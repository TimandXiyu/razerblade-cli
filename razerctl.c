// SPDX-License-Identifier: GPL-2.0-only
// razer-cli - Razer Blade 16 control over hidraw.
// Protocol derived from rnd-ash/razer-laptop-control (Ashcon Mohseninia), GPL-2.0.
// razerctl - Razer Blade 16 (1532:02b7) fan/perf control via hidraw.
// Protocol verified against rnd-ash/razer-laptop-control driver + by-ear test.
// Manual fan REQUIRES: power-mode cmd (0x0d/0x02) with manual flag (arg3=1),
// applied to BOTH fan zones (arg1=0x01 and 0x02), then rpm cmd (0x0d/0x01).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/file.h>           // flock (single-instance guard)
#include <errno.h>
#include <dlfcn.h>              // dlopen libnvidia-api.so / libnvidia-ml.so (dGPU undervolt page)
#include <stdint.h>
#include <dirent.h>             // dGPU PCI auto-discovery (address can change across boots)
#include <sys/stat.h>          // mkdir for the undervolt profile
#include <sys/prctl.h>         // raise cap_sys_admin into the AMBIENT set (sudo-less GPU write)
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/hidraw.h>
#include <sys/xattr.h>          // detect file-cap on the installed copy for the self-heal re-exec
#ifndef CAP_SYS_ADMIN
#define CAP_SYS_ADMIN 21
#endif
#define INSTALLED_BIN "/usr/local/bin/razerctl"
// PATH often resolves `razerctl` to the uncapped build artifact in the repo dir
// (caps drop on every rebuild and `make install` only caps the installed copy).
// That binary starts with no cap_sys_admin in PERMITTED -> the dGPU clock write
// fails -137. Self-heal: if we lack the cap but the installed copy has a file-cap
// xattr, transparently re-exec it (once, guarded by RZ_REEXEC) so the user always
// gets a working binary no matter which path they typed.
static void maybe_reexec_capped(char**argv){
    if(getenv("RZ_REEXEC")) return;                       // already handed off once
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct d[2]={{0,0,0},{0,0,0}};
    if(syscall(SYS_capget,&h,d)==0 && (d[0].permitted&(1u<<CAP_SYS_ADMIN))) return; // we already have it
    if(geteuid()==0) return;                              // root needs no cap
    char self[4096]={0}; ssize_t sl=readlink("/proc/self/exe",self,sizeof self-1); if(sl>0)self[sl]=0;
    if(!strcmp(self,INSTALLED_BIN)) return;               // we ARE the installed copy -> don't loop
    if(getxattr(INSTALLED_BIN,"security.capability",NULL,0)<=0) return; // installed copy uncapped/missing
    setenv("RZ_REEXEC","1",1);
    execv(INSTALLED_BIN,argv);                            // hand off; on failure fall through and run as-is
}
// libnvidia-api.so exec's a privileged helper for clock writes; effective-only
// file caps are lost across that exec, so the GPU write fails without sudo. Fix:
// move cap_sys_admin (held in permitted via file-cap +epi) into INHERITABLE then
// AMBIENT, so it survives the helper exec. No-op as root / when uncapped.
static void raise_ambient_sysadmin(void){
    int dbg=!!getenv("RZ_CAPDEBUG");
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct d[2]={{0,0,0},{0,0,0}};
    if(syscall(SYS_capget,&h,d)!=0){ if(dbg)fprintf(stderr,"capget fail %d\n",errno); return; }
    if(dbg)fprintf(stderr,"caps: eff=%08x perm=%08x inh=%08x\n",d[0].effective,d[0].permitted,d[0].inheritable);
    d[0].inheritable |= (1u<<CAP_SYS_ADMIN);
    if(syscall(SYS_capset,&h,d)!=0){ if(dbg)fprintf(stderr,"capset fail %d (%s)\n",errno,strerror(errno)); return; }
    int r=prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_RAISE,CAP_SYS_ADMIN,0,0);
    if(dbg)fprintf(stderr,"ambient raise ret=%d errno=%d  is_set=%ld\n",r,errno,
        (long)prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_IS_SET,CAP_SYS_ADMIN,0,0));
}
#define L 91                    // Razer HID feature-report length, in bytes
static unsigned char TID=0x1f;
// Razer HID feature report layout (L bytes):
//   [0]=report-id(0)  [2]=transaction-id(TID)  [6]=data-size  [7]=command-class
//   [8]=command-id    [9..]=args               [89]=CRC (xor of bytes 3..88)
// Command class/id pairs used below:
//   0x0d/0x02 set power-mode/fan-enable   0x0d/0x01 set fan rpm
//   0x0d/0x82 get power-mode   0x0d/0x81 get fan setpoint   0x0d/0x88 get fan tach
//   0x03/0x03 kbd brightness   0x03/0x0b kbd row colors     0x03/0x0a kbd commit
static void build(unsigned char*b,unsigned char cls,unsigned char cmd,unsigned char ds,const unsigned char*a,int n){
    memset(b,0,L); b[2]=TID; b[6]=ds; b[7]=cls; b[8]=cmd;
    for(int i=0;i<n;i++) b[9+i]=a[i];
    unsigned char c=0; for(int i=2;i<88;i++) c^=b[1+i]; b[89]=c;
}
static int xfer(int fd,unsigned char*b,unsigned char*in){
    if(ioctl(fd,HIDIOCSFEATURE(L),b)<0) return -1; usleep(60000);
    unsigned char tmp[L]; if(!in)in=tmp; memset(in,0,L); in[0]=0;
    if(ioctl(fd,HIDIOCGFEATURE(L),in)<0) return -2; return in[1];
}
static int snd(int fd,unsigned char cls,unsigned char cmd,unsigned char ds,const unsigned char*a,int n){
    unsigned char b[L]; build(b,cls,cmd,ds,a,n); return xfer(fd,b,NULL);
}
static int open_dev(char*chosen){
    for(int n=0;n<32;n++){ char p[64]; snprintf(p,sizeof p,"/dev/hidraw%d",n);
        int fd=open(p,O_RDWR); if(fd<0) continue;
        struct hidraw_devinfo di;
        if(ioctl(fd,HIDIOCGRAWINFO,&di)==0 && di.vendor==0x1532 && di.product==0x02b7){
            unsigned char b[L],in[L],z=0; build(b,0x00,0x81,0x02,&z,0);
            if(xfer(fd,b,in)==0x02){ if(chosen)strcpy(chosen,p); return fd; } }
        close(fd);
    }
    return -1;
}
// ---- reads ----
static int get_pmode(int fd){ unsigned char b[L],in[L],a[2]={0,1};
    build(b,0x0d,0x82,0x04,a,2); if(xfer(fd,b,in)!=0x02) return -1; return in[11]; }
static int get_fan_setpoint(int fd){ unsigned char b[L],in[L],a[2]={0,1};
    build(b,0x0d,0x81,0x04,a,2); if(xfer(fd,b,in)!=0x02) return -1; return in[11]*100; }
static int get_tach(int fd,int zone){ // 0x88 real tach (ramps ~40-50s; encoded /100)
    unsigned char b[L],in[L],a[2]={0,(unsigned char)zone};
    build(b,0x0d,0x88,0x04,a,2); if(xfer(fd,b,in)!=0x02) return -1; return in[11]*100;
}
// ---- writes ----
static int set_fan(int fd,int rpm){           // verified working sequence
    int sp=rpm/100; int m=get_pmode(fd); if(m<0||m>=4) m=0;
    // rpm write args[0] must be 0x01 (Synapse USB capture 2026-06-12); 0x00 made the
    // EC take the setpoint but not reliably settle the fan at it
    unsigned char z1e[4]={0,1,(unsigned char)m,1}, z1r[3]={1,1,(unsigned char)sp};
    unsigned char z2e[4]={0,2,(unsigned char)m,1}, z2r[3]={1,2,(unsigned char)sp};
    snd(fd,0x0d,0x82,0x04,(unsigned char[]){0,1,0,0},4); // get pwr
    int ok=0;
    ok|=snd(fd,0x0d,0x02,0x04,z1e,4)!=0x02;             // enable manual z1
    ok|=snd(fd,0x0d,0x01,0x03,z1r,3)!=0x02;             // rpm z1
    ok|=snd(fd,0x0d,0x02,0x04,z2e,4)!=0x02;             // enable manual z2
    ok|=snd(fd,0x0d,0x01,0x03,z2r,3)!=0x02;             // rpm z2
    return ok?-1:0;
}
static int set_fan_auto(int fd){
    int m=get_pmode(fd); if(m<0||m>=4) m=0;
    unsigned char z1[4]={0,1,(unsigned char)m,0}, z2[4]={0,2,(unsigned char)m,0};
    int ok=0; ok|=snd(fd,0x0d,0x02,0x04,z1,4)!=0x02; ok|=snd(fd,0x0d,0x02,0x04,z2,4)!=0x02;
    return ok?-1:0;
}
static int set_pmode(int fd,int mode){ // 0=balanced 1=gaming 2=creator. EC perf-mode 0x0d/0x02 to BOTH zones.
    if(mode<0||mode>2) return -1;                   // Custom (4) removed 2026-06-19 (unreliable, no GPU-TDP effect).
    int sp=get_fan_setpoint(fd); unsigned char flag = sp>0?1:0;
    unsigned char z1[4]={1,1,(unsigned char)mode,flag}, z2[4]={1,2,(unsigned char)mode,flag};
    int ok=0; ok|=snd(fd,0x0d,0x02,0x04,z1,4)!=0x02; ok|=snd(fd,0x0d,0x02,0x04,z2,4)!=0x02;
    if(ok) return -1;
    if(get_pmode(fd)!=mode) return -2;
    // GPU TDP on Linux is governed by Dynamic Boost (nvidia-powerd), NOT the EC perf-mode: powerd OFF caps the
    // dGPU at its ~80W default; ON lets the driver boost to ~175W. (nvidia-smi -pl is ignored/clamped on this Ada
    // laptop -- verified 2026-06-19.) So Creator parks the GPU at the 80W eco floor; Balanced/Gaming enable boost
    // and the EC mode then differentiates them (~118W vs ~165W under load). sudo-less via polkit (same as 'd' key).
    system(mode==2 ? "/usr/bin/systemctl stop nvidia-powerd 2>/dev/null"
                   : "/usr/bin/systemctl start nvidia-powerd 2>/dev/null");
    return 0;
}
// ---- battery charge limit (opcode from Synapse USB capture 2026-06-12) ----
static int get_charge_limit(int fd){ // raw byte: pct|0x80 when enabled, plain pct when disabled; <0 = error
    unsigned char b[L],in[L],a[1]={0};
    build(b,0x07,0x8f,0x01,a,1); if(xfer(fd,b,in)!=0x02) return -1; return in[9]; }
static int set_charge_limit(int fd,int raw){ // raw = pct|0x80 to enable, pct (bit7 clear) to disable-keep-value
    unsigned char a[1]={(unsigned char)raw};
    if(snd(fd,0x07,0x12,0x01,a,1)!=0x02) return -1;     // 1) setter (0x07/0x12)
    (void)get_charge_limit(fd);                          // 2) status read -- Synapse reads 0x07/0x8f between set & commit
    unsigned char c[1]={0x02};                           // 3) COMMIT/APPLY: Synapse sends 0x07/0x0f arg 0x02 after EVERY
    return snd(fd,0x07,0x0f,0x01,c,1)==0x02?0:-1; }      // battery write (decoded 2026-06-20). Without it the EC stores the value but never applies it -> charging never stops.
static int kbd_off(int fd){ unsigned char a[3]={0x01,0x05,0x00}; return snd(fd,0x03,0x03,0x03,a,3)==0x02?0:-1; }
static int kbd_color(int fd,int r,int g,int b){ // paint all 6 rows, commit, brightness 30%
    for(int row=0;row<6;row++){
        unsigned char a[52]; memset(a,0,sizeof a);
        a[0]=0xff; a[1]=(unsigned char)row; a[3]=0x0f;
        for(int k=0;k<15;k++){ a[7+k*3]=(unsigned char)r; a[7+k*3+1]=(unsigned char)g; a[7+k*3+2]=(unsigned char)b; }
        if(snd(fd,0x03,0x0b,0x34,a,52)!=0x02) return -1;
    }
    unsigned char cm[2]={0x05,0x00}; snd(fd,0x03,0x0a,0x02,cm,2);   // commit custom frame
    unsigned char br[3]={0x01,0x05,76}; snd(fd,0x03,0x03,0x03,br,3); // 30% = 76/255
    return 0;
}
// Keyboard backlight presets (index 0 = off; cycled by TUI 'k', named by CLI `kbd`).
static const struct { const char*name; int r,g,b; } KBD[] = {
    {"off",-1,-1,-1}, {"white",255,255,255}, {"red",255,0,0}, {"purple",128,0,128}, {"green",0,255,0}
};
#define NKBD ((int)(sizeof KBD / sizeof KBD[0]))
static const char* modename(int m){return m==0?"Balanced":m==1?"Gaming":m==2?"Creator":"?";}
// Restart KWin so it drops the dGPU after undocking -> dGPU returns to D3cold.
// Runs as the logged-in user (needs the Wayland session env); brief screen flicker.
static int reclaim_dgpu(void){
    return system("setsid sh -c 'KWIN_DRM_DEVICES=/dev/dri/igpu kwin_wayland --replace >/dev/null 2>&1 &'");
}
// ---------- TUI ----------
static struct termios orig;
static char obuf[1<<15];
static void raw_on(){ tcgetattr(0,&orig); struct termios t=orig; t.c_lflag&=~(ICANON|ECHO);
    t.c_cc[VMIN]=1; t.c_cc[VTIME]=0; tcsetattr(0,TCSANOW,&t);
    setvbuf(stdout,obuf,_IOFBF,sizeof obuf);   // whole frame -> one write() (no per-line flush flicker)
    printf("\033[2J\033[?25l"); }              // one full clear on entry; frames then redraw in place
static void raw_off(){ tcsetattr(0,TCSANOW,&orig); printf("\033[?25h\033[0m\n"); }
static void rds(const char*p,char*o,int n){ o[0]=0; FILE*f=fopen(p,"r"); if(!f)return;
    if(fgets(o,n,f)){ int l=strlen(o); if(l&&o[l-1]==0x0a)o[l-1]=0; } fclose(f); }
// Discover the dGPU PCI address at runtime (it can shift across boots, e.g. with
// MUX-mode changes) instead of hardcoding 0000:01:00.0. Scan for the NVIDIA
// (0x10de) VGA/3D controller; cache the result. Falls back to the usual slot.
static const char* dgpu_pci(void){
    static char addr[20]=""; if(addr[0]) return addr;
    DIR*d=opendir("/sys/bus/pci/devices");
    if(d){ struct dirent*e; char p[300],v[16],c[16];
        while((e=readdir(d))){ if(e->d_name[0]=='.')continue;
            snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/vendor",e->d_name); rds(p,v,sizeof v);
            if(strncmp(v,"0x10de",6))continue;
            snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/class",e->d_name); rds(p,c,sizeof c);
            if(!strncmp(c,"0x0300",6)||!strncmp(c,"0x0302",6)){ strncpy(addr,e->d_name,sizeof addr-1); break; } }
        closedir(d); }
    if(!addr[0]) strcpy(addr,"0000:01:00.0");
    return addr;
}
static void dgpu_path(char*buf,int n,const char*suffix){ snprintf(buf,n,"/sys/bus/pci/devices/%s/%s",dgpu_pci(),suffix); }
static long rdl(const char*p){FILE*f=fopen(p,"r");if(!f)return -1;long v=-1;if(fscanf(f,"%ld",&v)!=1)v=-1;fclose(f);return v;}
static double batt_watts(int*ac){
    long c=rdl("/sys/class/power_supply/BAT0/current_now");
    long v=rdl("/sys/class/power_supply/BAT0/voltage_now");
    *ac=(int)rdl("/sys/class/power_supply/AC0/online");
    return (c>0&&v>0)?(c/1e6)*(v/1e6):0.0;
}
static double mono(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static long long rdll(const char*p){FILE*f=fopen(p,"r");if(!f)return -1;long long v=-1;if(fscanf(f,"%lld",&v)!=1)v=-1;fclose(f);return v;}
static double rapl_w(const char*path,long long*prev,double*pts){
    long long e=rdll(path); double t=mono();
    if(e<0) return -1;
    if(*prev<0){*prev=e;*pts=t;return 0;}
    double dt=t-*pts; long long de=e-*prev;
    if(de<0) de+=262143328850LL;  /* wrap = max_energy_range_uj for this RAPL domain (pkg & core share it on this CPU) */
    *prev=e;*pts=t; return dt>0?(de/1e6)/dt:0;
}
static int pkg_temp(void){
    static char path[160]; static int done=0;
    if(!done){ done=1; path[0]=0;
        for(int h=0;h<24&&!path[0];h++){ char np[96]; snprintf(np,sizeof np,"/sys/class/hwmon/hwmon%d/name",h);
            char nm[32]; rds(np,nm,sizeof nm); if(strcmp(nm,"coretemp")) continue;
            for(int t=1;t<48;t++){ char lp[160]; snprintf(lp,sizeof lp,"/sys/class/hwmon/hwmon%d/temp%d_label",h,t);
                char lab[40]; rds(lp,lab,sizeof lab); if(strstr(lab,"Package")){ snprintf(path,sizeof path,"/sys/class/hwmon/hwmon%d/temp%d_input",h,t); break; } } } }
    if(!path[0])return -1; long long v=rdll(path); return v<0?-1:(int)(v/1000);
}
// ---------- custom fan curve (temp -> rpm, EMA-smoothed) ----------
// Fan speed is driven purely by CPU package temp + dGPU temp. Each maps through a linear
// ramp from a floor temp (pre-ramp -> FAN_RPM_ENGAGE) to its ALARM temp (-> FAN_RPM_MAX); the (shared)
// fans take the LOUDER of the two demands. Temps are EMA-smoothed asymmetrically (ramp up
// fast, spin down slow) to track load without hunting. At/above an alarm temp the RAW
// (unsmoothed) reading forces full speed at once -- safety is never smoothed.
#define FAN_RPM_IDLE   2200   // flat idle rpm below the pre-ramp (2026-06-10, ex 2000/2500)
#define FAN_RPM_ENGAGE 2500   // rpm reached at the main floor temp (T_LO)
#define FAN_RPM_MAX    4800   // real ceiling: EC caps manual fan ~4700rpm; 5300 command = identical tach (tested 2026-06-14). 5100 seen was firmware-auto under load
#define PRE_T_BAND     10     // pre-ramp width: (T_LO-10)..T_LO eases IDLE->ENGAGE
                              // (+30 rpm/C -- inaudible by design; CPU 55-65C, GPU 60-70C)
#define CPU_T_LO 65      // CPU: fan floor / engage temp (C) -- flat 2500 below
#define CPU_T_HI 90      // CPU: ALARM -> full speed (C)
#define GPU_T_LO 70      // GPU: fan floor / engage temp (C) -- flat 2500 below
#define GPU_T_HI 82      // GPU: ALARM -> full speed (C)
#define EMA_UP   0.50    // smoothing weight when a temp is RISING  (responsive)
#define EMA_DOWN 0.25    // smoothing weight when a temp is FALLING (spin-down speed)
#define FAN_STEP 150     // min rpm change before issuing a new HID write (hysteresis)
#define FAN_LOOP_S 2     // poll interval (s)
static int dgpu_awake(void){ char s[16],p[300]; dgpu_path(p,sizeof p,"power_state"); rds(p,s,sizeof s); return !strcmp(s,"D0"); }
// dGPU temp in C, or -1 if the dGPU is asleep/unreadable. Only shells out to nvidia-smi when
// the GPU is ALREADY D0 -- an idle dGPU stays in D3cold and we must never wake it just to poll.
static int gpu_temp(void){
    if(!dgpu_awake()) return -1;
    // `timeout 2` so a D0-but-wedged dGPU (driver hiccup) can't hang nvidia-smi and stall the TUI.
    FILE*f=popen("/usr/bin/timeout 2 /usr/bin/nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null","r");
    if(!f) return -1; int t=-1; if(fscanf(f,"%d",&t)!=1) t=-1; pclose(f); return t;
}
// Three segments: flat FAN_RPM_IDLE below (lo-PRE_T_BAND); a gentle pre-ramp
// IDLE->ENGAGE across (lo-PRE_T_BAND)..lo; then the main ramp ENGAGE->MAX at the
// alarm temp. The pre-ramp keeps the idle->engage transition inaudible.
static int curve_rpm(double t,double lo,double hi){
    if(t<=lo-PRE_T_BAND) return FAN_RPM_IDLE;
    if(t<lo) return FAN_RPM_IDLE +
        (int)((FAN_RPM_ENGAGE-FAN_RPM_IDLE)*(t-(lo-PRE_T_BAND))/PRE_T_BAND+0.5);
    if(t>=hi) return FAN_RPM_MAX;
    return FAN_RPM_ENGAGE + (int)((FAN_RPM_MAX-FAN_RPM_ENGAGE)*(t-lo)/(hi-lo)+0.5);
}
// One curve tick: read CPU+dGPU temps, advance the two EMA states (*ec/*eg, asymmetric: fast up,
// slow down), and return the target rpm (the LOUDER of the CPU/GPU demands; alarm forces FAN_RPM_MAX).
// Outputs the raw temps (*ctp/*gtp; gt=-1 if the dGPU is asleep) and whether an alarm tripped (*alm).
// Shared by the CLI `fancurve` loop and the TUI 'c' toggle so the two can never drift apart.
static int curve_step(double*ec,double*eg,int*ctp,int*gtp,int*alm){
    int ct=pkg_temp(), gt=gpu_temp(); *ctp=ct; *gtp=gt;
    if(ct>=0){ if(*ec<0)*ec=ct; else *ec += ((ct>*ec)?EMA_UP:EMA_DOWN)*(ct-*ec); }
    if(gt>=0){ if(*eg<0)*eg=gt; else *eg += ((gt>*eg)?EMA_UP:EMA_DOWN)*(gt-*eg); } else *eg=-1;
    *alm=(ct>=CPU_T_HI)||(gt>=GPU_T_HI);             // raw temps; gt=-1 (asleep) can't trip it
    int rc=(*ec>=0)?curve_rpm(*ec,CPU_T_LO,CPU_T_HI):FAN_RPM_IDLE;
    int rg=(*eg>=0)?curve_rpm(*eg,GPU_T_LO,GPU_T_HI):FAN_RPM_IDLE;
    int t=rc>rg?rc:rg; if(*alm)t=FAN_RPM_MAX; return (t/100)*100;
}
static volatile sig_atomic_t curve_stop=0;
static void curve_sigint(int s){ (void)s; curve_stop=1; }
// Standalone foreground curve loop (CLI `fancurve`); the TUI 'c' toggle reuses curve_step() directly.
static int fan_curve(int fd){
    signal(SIGINT,curve_sigint);
    printf("fan curve: CPU %d->%dC  GPU %d->%dC  =>  %d-%d rpm   (EMA up=%.2f down=%.2f)\n",
           CPU_T_LO,CPU_T_HI,GPU_T_LO,GPU_T_HI,FAN_RPM_IDLE,FAN_RPM_MAX,EMA_UP,EMA_DOWN);
    printf("alarm = full speed at CPU>=%dC or GPU>=%dC. Ctrl-C to stop (fans return to AUTO).\n",CPU_T_HI,GPU_T_HI);
    double ec=-1,eg=-1; int applied=-1;
    while(!curve_stop){
        int ct,gt,alarm; int target=curve_step(&ec,&eg,&ct,&gt,&alarm);
        if(applied<0||alarm||abs(target-applied)>=FAN_STEP){ if(set_fan(fd,target)==0) applied=target; }
        char gb[24]; if(gt>=0)snprintf(gb,sizeof gb,"%3dC(ema%5.1f)",gt,eg); else snprintf(gb,sizeof gb,"asleep       ");
        printf("\rCPU %3dC(ema%5.1f)  GPU %s  -> %4d rpm %s ",ct,ec,gb,applied,alarm?"[ALARM]":"       ");
        fflush(stdout);
        for(int i=0;i<FAN_LOOP_S*10 && !curve_stop;i++) usleep(100000);   // sleep, stay Ctrl-C responsive
    }
    printf("\nrestoring fan to AUTO...\n"); set_fan_auto(fd); return 0;
}
static double cpu_busy(void){ static long long pt=-1,pi=-1;
    FILE*f=fopen("/proc/stat","r"); if(!f)return -1; char c[8]; long long u,n,sy,idle,io,ir,si,st;
    if(fscanf(f,"%7s %lld %lld %lld %lld %lld %lld %lld %lld",c,&u,&n,&sy,&idle,&io,&ir,&si,&st)!=9){fclose(f);return -1;} fclose(f);
    long long tot=u+n+sy+idle+io+ir+si+st, idl=idle+io;
    if(pt<0){pt=tot;pi=idl;return 0;}
    long long dt=tot-pt,di=idl-pi; pt=tot;pi=idl; return dt>0?100.0*(dt-di)/dt:0;
}
static double deep_res(int ncpu){ static long long pv=-1; static double pts=-1;
    long long sum=0; for(int i=0;i<ncpu;i++){ char p[128]; snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpuidle/state3/time",i); long long v=rdll(p); if(v>0)sum+=v; }
    double t=mono(); if(pv<0){pv=sum;pts=t;return 0;} double dt=t-pts; long long d=sum-pv; pv=sum;pts=t;
    return dt>0?100.0*(d/1e6)/(dt*ncpu):0;
}
// Live CPU frequency: mean across all online cores + the single peak core (scaling_cur_freq, kHz->MHz).
static void cpu_freq(int ncpu,int*mean,int*max){
    long sum=0,mx=0; int cnt=0;
    for(int i=0;i<ncpu;i++){ char p[128]; snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",i);
        long f=rdl(p); if(f<=0) continue; sum+=f; if(f>mx)mx=f; cnt++; }
    *mean = cnt ? (int)(sum/cnt/1000) : -1;
    *max  = mx  ? (int)(mx/1000)     : -1;
}
// ---- EPP (CPU energy-vs-performance bias; intel_pstate HWP, raw 0-255: 0=max perf, 255=max powersave) ----
// sysfs echoes named tiers for the round values; map those back to the raw number.
#define EPP_PATH0 "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference"
static const int EPP_PRESET[]={0,32,64,96,128,160,192,224,255};   // knobs: 0..255 step 32 (+255 cap)
#define NEPP ((int)(sizeof EPP_PRESET / sizeof EPP_PRESET[0]))
#define NBATT 4   // battery charge-limit cycle: off/60/70/80
static int epp_named(const char*s){ // standard intel_pstate tier name -> raw value, else -1
    if(!strcmp(s,"performance"))return 0;        if(!strcmp(s,"balance_performance"))return 128;
    if(!strcmp(s,"balance_power"))return 192;     if(!strcmp(s,"power"))return 255;     return -1; }
static int get_epp(char*name,int n){ // returns raw 0-255 (best effort); name <- sysfs string
    char s[24]; rds(EPP_PATH0,s,sizeof s); if(name) snprintf(name,n,"%s",s[0]?s:"?");
    if(!s[0]) return -1; int k=epp_named(s); if(k>=0) return k;
    int v=atoi(s); return (v>=0&&v<=255)?v:-1; }
static int set_epp(int v){ // write EPP (raw 0-255) to every cpu. sudo-less IF energy_performance_preference
    if(v<0)v=0; if(v>255)v=255;  // is world-writable (see /etc/tmpfiles.d/epp-write.conf); 0=ok, -1=no write
    int n=(int)sysconf(_SC_NPROCESSORS_ONLN), ok=-1;
    for(int i=0;i<n;i++){ char p[128]; snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference",i);
        FILE*f=fopen(p,"w"); if(!f) continue; fprintf(f,"%d\n",v); if(fclose(f)==0) ok=0; }
    return ok; }
static int epp_nearest(int v){ int bi=0,bd=1<<30; for(int i=0;i<NEPP;i++){ int d=v-EPP_PRESET[i]; if(d<0)d=-d; if(d<bd){bd=d;bi=i;} } return bi; }
// ============================================================================
// dGPU per-point V/F-curve UNDERVOLT  (NvAPI via libnvidia-api.so)
// Mechanism + struct offsets ported from the python nvapi_vf.py (verified on
// this Ada 4080). Function IDs reverse-engineered by nvcurve. NvAPI exposes the
// real per-point curve + live voltage that NVML cannot.
// ============================================================================
#define NVF_INIT   0x0150E828u
#define NVF_ENUM   0xE5AC921Fu
#define NVF_NAME   0xCEEE8E9Fu
#define NVF_VFP    0x21537AD4u   // GetVFPCurve: base curve
#define NVF_MASK   0x507B4B59u   // GetClockBoostMask
#define NVF_CT     0x23F1B133u   // GetClockBoostTable: per-point offsets
#define NVF_VOLT   0x465F9BCFu   // GetCurrentVoltage
#define NVF_SETCT  0x0733E009u   // SetClockBoostTable: write per-point offsets
#define VFP_SIZE 0x1C28
#define VFP_BASE 0x48
#define VFP_STRIDE 0x1C
#define CT_SIZE  0x2420
#define CT_BASE  0x44
#define CT_STRIDE 0x24
#define CT_DELTA 0x14
#define MASK_SIZE 0x182C
#define VOLT_SIZE 0x4C
#define VOLT_OFF  0x28
#define NVPTS 128
typedef int (*nvfn0)(void);
typedef int (*nvfn2)(void*,void*);
static struct {
    void *lib; void*(*qi)(unsigned); void *gpu; int ok; char name[96];
    int n; int freq[NVPTS]; int volt[NVPTS]; int isgpu[NVPTS];   // base curve (kHz,uV)
} NV={0};
static unsigned char NVMASK[32];

static void* nvq(unsigned fid){ return NV.qi?(void*)NV.qi(fid):NULL; }
static int nvcall(unsigned fid,void*buf,int size,int ver){
    void*p=nvq(fid); if(!p) return -1;
    *(uint32_t*)buf=((unsigned)ver<<16)|size;
    return ((nvfn2)p)(NV.gpu,buf);
}
static int nv_get_mask(void){
    unsigned char b[MASK_SIZE]; memset(b,0,sizeof b);
    for(int i=4;i<36;i++) b[i]=0xFF;
    if(nvcall(NVF_MASK,b,MASK_SIZE,1)!=0) return -1;
    memcpy(NVMASK,b+4,32); return 0;
}
static int nv_read_curve(void){
    if(nv_get_mask()!=0) return -1;
    unsigned char v[VFP_SIZE]; memset(v,0,sizeof v); for(int i=0;i<32;i++) v[4+i]=NVMASK[i];
    if(nvcall(NVF_VFP,v,VFP_SIZE,1)!=0) return -1;
    unsigned char c[CT_SIZE]; memset(c,0,sizeof c); for(int i=0;i<32;i++) c[4+i]=NVMASK[i];
    if(nvcall(NVF_CT,c,CT_SIZE,1)!=0) return -1;
    NV.n=0; int in_mem=0;
    for(int i=0;i<NVPTS;i++){
        uint32_t f=*(uint32_t*)(v+VFP_BASE+i*VFP_STRIDE);
        uint32_t vo=*(uint32_t*)(v+VFP_BASE+i*VFP_STRIDE+4);
        uint32_t flags=*(uint32_t*)(c+CT_BASE+i*CT_STRIDE);
        if(f==0&&vo==0) continue;
        if(flags==1) in_mem=1;
        NV.freq[NV.n]=f; NV.volt[NV.n]=vo;
        NV.isgpu[NV.n]=(!in_mem && f>150000 && f<3300000 && vo>400000 && vo<1300000);
        NV.n++;
    }
    return 0;
}
static int nv_voltage_mv(void){
    if(!NV.ok) return -1;
    unsigned char b[VOLT_SIZE]; memset(b,0,sizeof b);
    if(nvcall(NVF_VOLT,b,VOLT_SIZE,1)!=0) return -1;
    return (int)(*(uint32_t*)(b+VOLT_OFF)/1000);
}
static int nv_init(void){
    NV.lib=dlopen("libnvidia-api.so.1",RTLD_NOW); if(!NV.lib) NV.lib=dlopen("libnvidia-api.so",RTLD_NOW);
    if(!NV.lib) return -1;
    NV.qi=(void*(*)(unsigned))dlsym(NV.lib,"nvapi_QueryInterface"); if(!NV.qi) return -1;
    void*ip=nvq(NVF_INIT); if(!ip||((nvfn0)ip)()!=0) return -1;
    void*ep=nvq(NVF_ENUM); if(!ep) return -1;
    void*gpus[64]={0}; int ng=0;
    ((nvfn2)ep)(gpus,&ng); if(ng<=0) return -1;
    NV.gpu=gpus[0];
    void*np=nvq(NVF_NAME); if(np){ char nb[256]={0}; ((nvfn2)np)(NV.gpu,nb); strncpy(NV.name,nb,sizeof NV.name-1); }
    if(nv_read_curve()!=0) return -1;
    NV.ok=1; return 0;
}
// interpolate freq(kHz) at voltage vuv over gpu-core curve (sorted ascending)
static int nv_interp(int vuv){
    int first=-1,last=-1;
    for(int i=0;i<NV.n;i++) if(NV.isgpu[i]){ if(first<0)first=i; last=i; }
    if(first<0) return 0;
    if(vuv<=NV.volt[first]) return NV.freq[first];
    if(vuv>=NV.volt[last])  return NV.freq[last];
    int prev=first;
    for(int i=first;i<=last;i++){ if(!NV.isgpu[i])continue;
        if(NV.volt[i]>=vuv){ int v0=NV.volt[prev],f0=NV.freq[prev],v1=NV.volt[i],f1=NV.freq[i];
            if(v1==v0) return f1; return f0+(int)((double)(vuv-v0)/(v1-v0)*(f1-f0)); }
        prev=i; }
    return NV.freq[last];
}
// write per-point undervolt: left-shift curve by mv above min_freq_mhz; <0 err
static int nv_write_uv(int mv,int minf_mhz){
    if(!NV.ok) return -1;
    unsigned char c[CT_SIZE]; memset(c,0,sizeof c); for(int i=0;i<32;i++) c[4+i]=NVMASK[i];
    if(nvcall(NVF_CT,c,CT_SIZE,1)!=0) return -1;        // read current table as base
    *(uint32_t*)c=(1u<<16)|CT_SIZE;
    for(int i=4;i<36;i++) c[i]=0;                        // clear mask -> sparse
    int dv=mv*1000, floor=minf_mhz*1000;
    for(int i=0;i<NV.n;i++){
        if(!NV.isgpu[i]) continue;
        int delta=0;
        if(NV.freq[i]>=floor){ int t=nv_interp(NV.volt[i]+dv); delta=t-NV.freq[i];
            if(delta>1000000)delta=1000000; if(delta<-1000000)delta=-1000000; }
        c[4+(i/8)] |= (1<<(i%8));
        *(int32_t*)(c+CT_BASE+i*CT_STRIDE+CT_DELTA)=delta;
    }
    void*sp=nvq(NVF_SETCT); if(!sp) return -1;
    return ((nvfn2)sp)(NV.gpu,c);
}
static int nv_reset(void){ return nv_write_uv(0,0); }
static int nv_uv_count(int mv,int minf_mhz,int*peak_mhz){      // dry preview
    int dv=mv*1000,floor=minf_mhz*1000,cnt=0,pk=0;
    for(int i=0;i<NV.n;i++){ if(!NV.isgpu[i]||NV.freq[i]<floor) continue;
        int d=nv_interp(NV.volt[i]+dv)-NV.freq[i]; if(d){cnt++; if(d>pk)pk=d;} }
    if(peak_mhz)*peak_mhz=pk/1000; return cnt;
}
// ---- undervolt profile persistence (~/.config/razerctl-uv.conf) ----
static void uv_path(char*b,int n){ const char*h=getenv("HOME"); snprintf(b,n,"%s/.config/razerctl-uv.conf",(h&&*h)?h:"/root"); }
static void uv_save(int mv,int minf,int maxf){
    const char*h=getenv("HOME"); char dir[256]; snprintf(dir,sizeof dir,"%s/.config",(h&&*h)?h:"/root"); mkdir(dir,0755);
    char p[300]; uv_path(p,sizeof p); FILE*f=fopen(p,"w");
    if(f){ fprintf(f,"uv_mv=%d\nmin_freq=%d\nmax_freq=%d\n",mv,minf,maxf); fclose(f); }
}
static int uv_load(int*mv,int*minf,int*maxf){
    char p[300]; uv_path(p,sizeof p); FILE*f=fopen(p,"r"); if(!f) return -1;
    char k[40]; int v; *mv=0;*minf=0;*maxf=0;
    while(fscanf(f,"%39[^=]=%d ",k,&v)==2){
        if(!strcmp(k,"uv_mv"))*mv=v; else if(!strcmp(k,"min_freq"))*minf=v; else if(!strcmp(k,"max_freq"))*maxf=v; }
    fclose(f); return 0;
}
// ---- NVML-lite (dlopen) for dGPU clock/power/temp on the undervolt page ----
static struct { void*lib; void*h; int ok;
    int(*clk)(void*,int,unsigned*); int(*pw)(void*,unsigned*); int(*tp)(void*,int,unsigned*);
} ML={0};
static int ml_init(void){
    ML.lib=dlopen("libnvidia-ml.so.1",RTLD_NOW); if(!ML.lib) ML.lib=dlopen("libnvidia-ml.so",RTLD_NOW);
    if(!ML.lib) return -1;
    int(*init)(void)=(int(*)(void))dlsym(ML.lib,"nvmlInit_v2"); if(!init||init()!=0) return -1;
    int(*gh)(unsigned,void**)=(int(*)(unsigned,void**))dlsym(ML.lib,"nvmlDeviceGetHandleByIndex_v2");
    if(!gh||gh(0,&ML.h)!=0) return -1;
    ML.clk=(int(*)(void*,int,unsigned*))dlsym(ML.lib,"nvmlDeviceGetClockInfo");
    ML.pw =(int(*)(void*,unsigned*))dlsym(ML.lib,"nvmlDeviceGetPowerUsage");
    ML.tp =(int(*)(void*,int,unsigned*))dlsym(ML.lib,"nvmlDeviceGetTemperature");
    ML.ok=1; return 0;
}
static int ml_core(void){ unsigned v=0; return (ML.ok&&ML.clk&&ML.clk(ML.h,0,&v)==0)?(int)v:-1; }   // NVML_CLOCK_GRAPHICS=0
static int ml_power(void){ unsigned v=0; return (ML.ok&&ML.pw&&ML.pw(ML.h,&v)==0)?(int)(v/1000):-1; }
static int ml_temp(void){ unsigned v=0; return (ML.ok&&ML.tp&&ML.tp(ML.h,0,&v)==0)?(int)v:-1; }     // NVML_TEMPERATURE_GPU=0
static void nv_lock_max(int maxf){ if(maxf>0&&ML.ok){ int(*lk)(void*,unsigned,unsigned)=(int(*)(void*,unsigned,unsigned))dlsym(ML.lib,"nvmlDeviceSetGpuLockedClocks"); if(lk) lk(ML.h,210,maxf); } }

// "on" = nvidia-powerd ACTIVE this boot. We toggle via start/stop (runtime, polkit-scoped to the unit),
// NOT enable/disable, so it's intentionally non-persistent: a reboot returns to powerd OFF (D3cold default).
static int powerd_enabled(void){ return system("/usr/bin/systemctl is-active --quiet nvidia-powerd 2>/dev/null")==0; }

// ---- arrow-key input: returns a logical key or a raw char ----
enum { K_UP=0x100,K_DOWN,K_LEFT,K_RIGHT,K_ENTER,K_ESC };
// Read ONE more byte of an in-flight escape sequence, waiting up to `us` for it.
// Uses raw read(0) -- NOT getchar() -- so it shares the same kernel buffer the
// main loop's select() polls. (Mixing getchar()'s stdio buffer with select() on
// the raw fd silently dropped arrow keys: getchar() slurped the whole ESC[A into
// stdio, select() saw an empty kernel buffer, and the arrow decoded as a bare ESC.)
static int seq_byte(int us){
    fd_set r; FD_ZERO(&r); FD_SET(0,&r); struct timeval tv={0,us};
    if(select(1,&r,0,0,&tv)<=0) return -1;
    unsigned char b; return read(0,&b,1)==1 ? b : -1;
}
static int readkey(void){
    unsigned char c; if(read(0,&c,1)!=1) return -1;
    if(c==27){   // ESC alone, or CSI arrow sequence -- 50ms grace so a terminal that
        int a=seq_byte(50000);            // splits ESC [ A across reads (SSH) isn't chopped
        if(a<0) return K_ESC;
        if(a=='['){ int b=seq_byte(50000);
            if(b=='A')return K_UP; if(b=='B')return K_DOWN;
            if(b=='C')return K_RIGHT; if(b=='D')return K_LEFT; return -1; }
        return K_ESC;
    }
    if(c=='\r'||c=='\n') return K_ENTER;
    return c;
}
#define ROWC(y,is,...) do{ printf("  %s ",(is)?"\033[1;36m▶":" "); printf(__VA_ARGS__); printf("\033[0m\033[K\n"); }while(0)
#define MROWS 10
#define DROWS 6

static int tui(int fd,const char*node){
    int manual=0,rpm=4000; char msg[160]="";
    int fanmode=2,alarm=0,capplied=-1; double ec=-1,eg=-1;  // fanmode 0=Auto 1=Manual 2=Curve (startup Curve)
    set_pmode(fd,0); kbd_off(fd);                            // startup defaults: Balanced + kbd off
    int pm=get_pmode(fd), sp=get_fan_setpoint(fd);
    long long pe=-1,ce=-1; double pts=0,cts=0; int ncpu=(int)sysconf(_SC_NPROCESSORS_ONLN); int mon=1;
    int kbi=0; int powerd_on=powerd_enabled(); int eppi=epp_nearest(get_epp(NULL,0)); int batti=0;
    int page=0, sel=0, dsel=0;                               // page 0=main 1=dGPU undervolt
    int uv_mv=0, minf=1000, maxf=0, nv_applied=0, nv_started=0; char nverr[80]="";
    static const int BP[NBATT]={0,60,70,80};
    // Telemetry is cached and refreshed at most once per POLL_S; redraws/keypresses reuse the
    // cache so navigation stays snappy and the delta metrics (W, busy%) keep a steady window.
    #define POLL_S 5.0
    int f1=-1,f2=-1,temp=-1,ac=-1,meanf=-1,maxf_f=-1; double bw=0,pkgw=-1,corew=-1,busy=-1,deep=-1;
    char gst[32]="?",gps[16]="?"; double last_poll=-1, last_curve=-1;
    raw_on();
    for(;;){
        int curve=(fanmode==2);
        // ---- telemetry (throttled to POLL_S) ----
        if(mon && page==0 && (last_poll<0 || mono()-last_poll>=POLL_S)){
            last_poll=mono();
            f1=get_tach(fd,1); f2=get_tach(fd,2); bw=batt_watts(&ac);
            { char pp[300]; dgpu_path(pp,sizeof pp,"power/runtime_status"); rds(pp,gst,sizeof gst);
              dgpu_path(pp,sizeof pp,"power_state"); rds(pp,gps,sizeof gps); }
            pkgw=rapl_w("/sys/class/powercap/intel-rapl:0/energy_uj",&pe,&pts);
            corew=rapl_w("/sys/class/powercap/intel-rapl:0:0/energy_uj",&ce,&cts);
            temp=pkg_temp(); busy=cpu_busy(); deep=deep_res(ncpu); cpu_freq(ncpu,&meanf,&maxf_f);
            powerd_on=powerd_enabled();
        }
        // Fan-curve tick is gated to FAN_LOOP_S regardless of how often the loop wakes (keypresses,
        // page redraws), so navigation never triggers an nvidia-smi spawn or a burst of EC writes.
        if(curve){ if(last_curve<0 || mono()-last_curve>=FAN_LOOP_S){ last_curve=mono();
            int ct,gt; rpm=curve_step(&ec,&eg,&ct,&gt,&alarm); (void)ct;(void)gt;
            if(capplied<0||alarm||abs(rpm-capplied)>=FAN_STEP){ if(set_fan(fd,rpm)==0) capplied=rpm; } } }
        else alarm=0;

        // ---- render ----
        if(page==0){
            char eppname[24]; int epp=get_epp(eppname,sizeof eppname);
            printf("\033[H\033[1;36m  razerctl\033[0m  Blade 16 (1532:02b7)  [%s]\033[K\n",node);
            printf("  --------------------------------------------\n");
            if(mon){
                printf("   Fan RPM  \033[1;32m%4d / %4d\033[0m   Batt \033[1;33m%s %.1fW\033[0m%s\033[K\n",f1,f2,ac?"AC":"BAT",bw,
                    alarm?"  \033[1;31m[ALARM]\033[0m":"");
                { int gone=(gst[0]==0); int asleep=gone||(strcmp(gst,"suspended")==0);
                  printf("   dGPU     %s%s\033[0m   CPU \033[1;33m%dC %.0fW\033[0m   busy \033[1;33m%.0f%%\033[0m\033[K\n",
                      asleep?"\033[1;32m":"\033[1;31m", gone?"absent ~0W":(asleep?"asleep ~0W":"AWAKE"),temp,pkgw,busy); }
            } else printf("   \033[1;90m-- monitor paused --\033[0m\033[K\n");
            printf("  --------------------------------------------\n");
            printf("   \033[1mSETTINGS\033[0m  \033[1;90m↑↓ select  ←→ change\033[0m\n");
            ROWC(0,sel==0,"Perf mode : \033[1;33m%-9s\033[0m %s",modename(pm),pm==2?"GPU eco ~80W":pm==1?"boost ~175W":"boost ~118W");
            ROWC(1,sel==1,"Fan       : \033[1;33m%-7s\033[0m %s",fanmode==0?"Auto":fanmode==1?"Manual":"Curve",
                 (fanmode==2)?"temp-driven":(fanmode==1?"manual rpm":"firmware"));
            ROWC(2,sel==2,"Fan RPM   : \033[1;33m%-5d\033[0m %s",rpm,fanmode==1?"":"\033[1;90m(Manual only)\033[0m");
            ROWC(3,sel==3,"Kbd light : \033[1;33m%s\033[0m",KBD[kbi].name);
            ROWC(4,sel==4,"CPU EPP   : \033[1;33m%-3d\033[0m (%s)",epp,eppname);
            ROWC(5,sel==5,"DynBoost  : %s\033[0m",powerd_on?"\033[1;32mON  (≤175W)":"\033[1;31mOFF (80W cap)");
            ROWC(6,sel==6,"Battery   : \033[1;33m%s\033[0m",BP[batti]?({static char bb[8];snprintf(bb,8,"%d%%",BP[batti]);bb;}):"Off (100%)");
            printf("  - - - - - - - - - - - - - - - - - - - - - - \n");
            ROWC(7,sel==7,"Monitoring: %s\033[0m",mon?"\033[1;32mON":"\033[1;90mpaused");
            ROWC(8,sel==8,"\033[1;36mdGPU undervolt ▸\033[0m  \033[1;90mEnter to open\033[0m");
            ROWC(9,sel==9,"Reclaim dGPU      \033[1;90mEnter: restart KWin\033[0m");
            printf("  --------------------------------------------\n");
            printf("   \033[1;90m↑↓ move   ←→ change   Enter open/toggle   q quit\033[0m\n");
        } else {
            if(!nv_started){ nv_started=1; if(nv_init()!=0) snprintf(nverr,sizeof nverr,"NvAPI init failed (libnvidia-api.so?)"); ml_init(); }
            int liveV=NV.ok?nv_voltage_mv():-1, core=ml_core(), pw=ml_power(), gt=ml_temp();
            int pk=0,cnt=NV.ok?nv_uv_count(uv_mv,minf,&pk):0;
            printf("\033[H\033[1;36m  razerctl · dGPU undervolt\033[0m  [%s]   \033[1;90m◂ Esc: back\033[0m\033[K\n",NV.ok?NV.name:"no NvAPI");
            printf("  --------------------------------------------\n");
            if(NV.ok){
                printf("   LIVE  core \033[1;33m%4dMHz\033[0m  volt \033[1;32m%dmV\033[0m  pwr \033[1;33m%dW\033[0m  temp \033[1;33m%dC\033[0m\033[K\n",
                    core<0?0:core, liveV<0?0:liveV, pw<0?0:pw, gt<0?0:gt);
            } else printf("   \033[1;31m%s\033[0m\033[K\n", nverr[0]?nverr:"NvAPI unavailable");
            printf("  --------------------------------------------\n");
            printf("   \033[1mUNDERVOLT\033[0m  \033[1;90m↑↓ select  ←→ change  Enter: Apply/Reset\033[0m\n");
            ROWC(0,dsel==0,"Undervolt : \033[1;33m%-3d mV\033[0m  \033[1;90mcurve left-shift\033[0m",uv_mv);
            ROWC(1,dsel==1,"Min freq  : \033[1;33m%-5d MHz\033[0m \033[1;90mstock below\033[0m",minf);
            ROWC(2,dsel==2,"Max freq  : \033[1;33m%-5s\033[0m \033[1;90mclock ceiling\033[0m",maxf?({static char mb[12];snprintf(mb,12,"%dMHz",maxf);mb;}):"off");
            printf("  - - - - - - - - - - - - - - - - - - - - - - \n");
            ROWC(3,dsel==3,"\033[1;32m[ Apply ]\033[0m  \033[1;90m%d pts, peak +%dMHz\033[0m",cnt,pk);
            ROWC(4,dsel==4,"[ Reset ]  \033[1;90mback to stock curve\033[0m");
            ROWC(5,dsel==5,"\033[1;36m◂ Back\033[0m      \033[1;90mEnter or Esc\033[0m");
            printf("   state: %s\033[0m\033[K\n", nv_applied?"\033[1;32mAPPLIED":"\033[1;90mnot applied");
            printf("  --------------------------------------------\n");
            printf("   \033[1;90m↑↓ move   ←→ change   Enter apply/reset/back   q quit\033[0m\n");
        }
        if(msg[0]) printf("\n   \033[32m%s\033[0m\033[K\n",msg);
        printf("\033[J");   // wipe any lines below the frame (page switch / msg cleared)
        fflush(stdout);

        // ---- input ----
        // Sleep only until the next thing actually falls due (telemetry @POLL_S, curve @FAN_LOOP_S,
        // dGPU page @1s live voltage); a keypress wakes us early regardless. This avoids a fixed
        // busy-redraw cadence (screen flicker / wasted work) while keeping refreshes on schedule.
        double wait;
        if(page){ wait=1.0; }
        else {
            wait=POLL_S;
            if(mon && last_poll>=0){ double w=last_poll+POLL_S-mono(); if(w<wait) wait=w; }
            if(curve){ double w=(last_curve<0)?0:last_curve+FAN_LOOP_S-mono(); if(w<wait) wait=w; }
            if(wait<0.1) wait=0.1; if(wait>POLL_S) wait=POLL_S;
        }
        struct timeval tv={(int)wait,(int)((wait-(int)wait)*1e6)};
        fd_set r; FD_ZERO(&r); FD_SET(0,&r);
        if(select(1,&r,0,0, (mon||curve||page)?&tv:NULL)<=0) continue;
        msg[0]=0; int k=readkey();

        if(page==0){
            if(k=='q'){ if(fanmode==2) set_fan_auto(fd); break; }
            else if(k==K_UP){ sel=(sel+MROWS-1)%MROWS; }
            else if(k==K_DOWN){ sel=(sel+1)%MROWS; }
            else if(k==K_ENTER){
                if(sel==7){ mon=!mon; pe=ce=-1; last_poll=-1; snprintf(msg,sizeof msg,mon?"monitoring ON":"monitoring paused"); }
                else if(sel==8){ page=1; dsel=0; }
                else if(sel==9){ reclaim_dgpu(); snprintf(msg,sizeof msg,"KWin restarted (reclaim dGPU)"); }
            }
            else if(k==K_LEFT||k==K_RIGHT){
                int dir=(k==K_RIGHT)?1:-1;
                if(sel==7){ mon=!mon; pe=ce=-1; last_poll=-1; snprintf(msg,sizeof msg,mon?"monitoring ON":"monitoring paused"); }
                else if(sel==0){ int w=(pm+dir+3)%3; set_pmode(fd,w); pm=get_pmode(fd); powerd_on=powerd_enabled();
                    snprintf(msg,sizeof msg,"mode -> %s",modename(pm)); }
                else if(sel==1){ fanmode=(fanmode+dir+3)%3;
                    if(fanmode==0){ manual=0; set_fan_auto(fd); capplied=-1; snprintf(msg,sizeof msg,"fan Auto"); }
                    else if(fanmode==1){ manual=1; set_fan(fd,rpm); snprintf(msg,sizeof msg,"fan Manual %d",rpm); }
                    else { manual=0; ec=eg=-1; capplied=-1; snprintf(msg,sizeof msg,"fan Curve (temp-driven)"); }
                    sp=get_fan_setpoint(fd); }
                else if(sel==2){ if(fanmode==1){ rpm+=dir*500; if(rpm>4800)rpm=4800; if(rpm<2000)rpm=2000;
                    set_fan(fd,rpm); sp=get_fan_setpoint(fd); snprintf(msg,sizeof msg,"rpm %d",rpm); }
                    else snprintf(msg,sizeof msg,"set Fan to Manual first"); }
                else if(sel==3){ kbi=(kbi+dir+NKBD)%NKBD; if(KBD[kbi].r<0)kbd_off(fd); else kbd_color(fd,KBD[kbi].r,KBD[kbi].g,KBD[kbi].b);
                    snprintf(msg,sizeof msg,"kbd -> %s",KBD[kbi].name); }
                else if(sel==4){ eppi=(eppi+dir+NEPP)%NEPP; int v=EPP_PRESET[eppi];
                    if(set_epp(v)==0) snprintf(msg,sizeof msg,"EPP -> %d (TLP resets on AC/DC)",v);
                    else snprintf(msg,sizeof msg,"EPP needs sudo razerctl"); }
                else if(sel==5){ int r2=system(powerd_on?"/usr/bin/systemctl stop nvidia-powerd 2>/dev/null":"/usr/bin/systemctl start nvidia-powerd 2>/dev/null");
                    powerd_on=powerd_enabled(); snprintf(msg,sizeof msg,r2?"boost toggle failed (polkit?)":"DynBoost -> %s",powerd_on?"ON":"OFF"); }
                else if(sel==6){ batti=(batti+dir+NBATT)%NBATT; int p=BP[batti]; int val=p?(p|0x80):0x41;
                    int r2=set_charge_limit(fd,val);
                    if(r2) snprintf(msg,sizeof msg,"charge-limit write FAILED");
                    else if(p) snprintf(msg,sizeof msg,"charge limit -> %d%%",p);
                    else snprintf(msg,sizeof msg,"charge limit OFF (100%%)"); }
            }
        } else {  // dGPU undervolt page
            if(k==K_ESC){ page=0; last_poll=-1; }
            else if(k=='q'){ if(fanmode==2) set_fan_auto(fd); break; }
            else if(k==K_UP){ dsel=(dsel+DROWS-1)%DROWS; }
            else if(k==K_DOWN){ dsel=(dsel+1)%DROWS; }
            else if(k==K_LEFT||k==K_RIGHT){
                int dir=(k==K_RIGHT)?1:-1;
                if(dsel==0){ uv_mv+=dir*5; if(uv_mv<0)uv_mv=0; if(uv_mv>150)uv_mv=150; }
                else if(dsel==1){ minf+=dir*15; if(minf<0)minf=0; if(minf>3000)minf=3000; }
                else if(dsel==2){ maxf+=dir*15; if(maxf<0)maxf=0; if(maxf>3105)maxf=3105; }
            }
            else if(k==K_ENTER){
                if(dsel==5){ page=0; last_poll=-1; }
                else if(!NV.ok) snprintf(msg,sizeof msg,"NvAPI unavailable — cannot apply");
                else if(dsel==3){ int r2=nv_write_uv(uv_mv,minf);
                    if(r2!=0) snprintf(msg,sizeof msg,"curve write FAILED (nvapi %d)",r2);
                    else { nv_applied=1; int pk,c=nv_uv_count(uv_mv,minf,&pk);
                        nv_lock_max(maxf); uv_save(uv_mv,minf,maxf);
                        snprintf(msg,sizeof msg,"APPLIED -%dmV >%dMHz (%d pts,+%dMHz) - saved, persists at login",uv_mv,minf,c,pk); } }
                else if(dsel==4){ nv_reset();
                    if(ML.ok){ int(*ul)(void*)=(int(*)(void*))dlsym(ML.lib,"nvmlDeviceResetGpuLockedClocks"); if(ul) ul(ML.h); }
                    nv_applied=0; uv_mv=0; maxf=0; { char p[300]; uv_path(p,sizeof p); unlink(p); }
                    snprintf(msg,sizeof msg,"reset to stock curve (profile cleared)"); }
            }
        }
    }
    if(NV.ok) nv_reset();   // leave the GPU at stock on exit (curve resets on reboot anyway)
    raw_off(); return 0;
}
// Single-instance guard. Two razerctl processes doing interleaved hidraw feature-reports
// on the same EC cross each other's responses -> corrupt readbacks / false "setter failed"
// (e.g. an open TUI vs a CLI invocation). Take a non-blocking exclusive flock at startup;
// the fd is intentionally kept open for the process lifetime (kernel drops the lock on exit,
// even on crash/kill -- no stale lock). Returns 0 if we hold it (or the lock is unusable, in
// which case we don't block the tool), -1 if another instance already holds it.
static int single_instance(void){
    int fd=open("/tmp/razerctl.lock",O_RDWR|O_CREAT,0666);
    if(fd<0) return 0;                                   // can't lock -> don't block functionality
    if(flock(fd,LOCK_EX|LOCK_NB)==0){
        char b[16]; int n=snprintf(b,sizeof b,"%d\n",(int)getpid());
        if(ftruncate(fd,0)==0){ ssize_t w=write(fd,b,n); (void)w; }
        return 0;                                        // fd leaked on purpose -> lock held until exit
    }
    if(errno==EWOULDBLOCK||errno==EAGAIN){
        char b[16]={0}; int other=0; if(pread(fd,b,sizeof b-1,0)>0) other=atoi(b);
        if(other>0) fprintf(stderr,"razerctl: another instance is already running (PID %d).\n",other);
        else        fprintf(stderr,"razerctl: another instance is already running.\n");
        fprintf(stderr,"Refusing to start -- concurrent EC access corrupts readings. Quit the other one first.\n");
        close(fd); return -1;
    }
    close(fd); return 0;                                 // unexpected flock error -> don't block
}
int main(int argc,char**argv){
    maybe_reexec_capped(argv);  // hand off to the installed capped copy if PATH gave us the uncapped build
    raise_ambient_sysadmin();   // sudo-less dGPU clock write (see helper above)
    // dGPU undervolt CLI (no EC/hidraw needed; NvAPI enumerates the GPU by device,
    // so it's immune to PCI-address changes across boots).
    if(argc>=2 && !strcmp(argv[1],"--uv-apply")){           // headless re-apply (systemd unit)
        int mv,minf,maxf; if(uv_load(&mv,&minf,&maxf)!=0){ printf("no undervolt profile\n"); return 0; }
        if(nv_init()!=0){ printf("dGPU absent (iGPU-only boot?) - undervolt skipped\n"); return 0; }  // graceful no-op
        int r=nv_write_uv(mv,minf); if(maxf>0){ ml_init(); nv_lock_max(maxf); }
        printf("uv-apply: -%dmV >%dMHz%s -> %s\n",mv,minf, maxf?" capped":"", r==0?"ok":"FAILED");
        return r==0?0:1;
    }
    if(argc>=2 && !strcmp(argv[1],"uv")){                   // razerctl uv <mv> <minf> [maxf] | reset
        if(nv_init()!=0){ fprintf(stderr,"NvAPI unavailable (libnvidia-api.so?)\n"); return 1; }
        if(argc>=3 && !strcmp(argv[2],"reset")){ nv_reset(); char p[300]; uv_path(p,sizeof p); unlink(p); printf("reset to stock, profile cleared\n"); return 0; }
        if(argc<4){ fprintf(stderr,"usage: razerctl uv <mV> <min_freq_MHz> [max_freq_MHz]\n       razerctl uv reset\n"); return 1; }
        int mv=atoi(argv[2]),minf=atoi(argv[3]),maxf=argc>4?atoi(argv[4]):0;
        int r=nv_write_uv(mv,minf);
        if(r!=0){ fprintf(stderr,"curve write FAILED (nvapi %d) - need cap_sys_admin or sudo\n",r); return 1; }
        if(maxf>0){ ml_init(); nv_lock_max(maxf); }
        uv_save(mv,minf,maxf); int pk,c=nv_uv_count(mv,minf,&pk);
        printf("applied -%dmV >%dMHz (%d pts, +%dMHz)%s; profile saved (persists at login)\n",mv,minf,c,pk,maxf?", capped":"");
        return 0;
    }
    if(single_instance()<0) return 3;
    char node[64]="?"; int fd=open_dev(node);
    if(fd<0){ fprintf(stderr,"razerctl: no responding 1532:02b7 hidraw (root? udev rule?)\n"); return 2; }
    if(argc==1) return tui(fd,node);
    if(!strcmp(argv[1],"nvtest")){
        if(nv_init()!=0){ printf("nv_init FAILED\n"); return 1; }
        int g=0; for(int i=0;i<NV.n;i++) if(NV.isgpu[i]) g++;
        printf("GPU: %s  pts=%d gpu-core=%d  liveV=%dmV\n",NV.name,NV.n,g,nv_voltage_mv());
        int pk; int c=nv_uv_count(50,1000,&pk);
        printf("-50mV>1000MHz preview: %d pts, peak +%dMHz\n",c,pk);
        return 0;
    }
    if(!strcmp(argv[1],"get")){
        char en[24]; int ev=get_epp(en,sizeof en);
        printf("node=%s perf=%s fan-setpoint=%d rpm (0=auto) epp=%d (%s) cpu=%dC\n",node,modename(get_pmode(fd)),get_fan_setpoint(fd),ev,en,pkg_temp());
    } else if(!strcmp(argv[1],"epp")){
        // CPU energy-vs-performance bias (intel_pstate HWP, raw 0-255). NOTE: TLP overrides this
        // on every AC/DC switch via CPU_ENERGY_PERF_POLICY_ON_AC/BAT -> manual set is a live nudge.
        char en[24]; int ev=get_epp(en,sizeof en);
        if(argc==2){
            printf("epp=%d (%s)   presets: 0 32 64 96 128 160 192 224 255  (0=max perf, 255=max powersave)\n",ev,en);
            printf("note: TLP resets this on the next AC<->DC change (CPU_ENERGY_PERF_POLICY_ON_AC/BAT)\n");
        } else { int v=atoi(argv[2]); if(v<0)v=0; if(v>255)v=255;
            int r=set_epp(v); char en2[24]; int nv=get_epp(en2,sizeof en2);
            printf("%s epp -> %d (now %d / %s); TLP will reset it on the next AC/DC switch\n", r==0?"ok":"FAILED", v, nv, en2);
            if(r!=0) fprintf(stderr,"  (energy_performance_preference not writable; is /etc/tmpfiles.d/epp-write.conf applied?)\n");
        }
    } else if(!strcmp(argv[1],"fan")&&argc==3){
        if(!strcmp(argv[2],"auto")) printf("%s\n",set_fan_auto(fd)==0?"fan auto":"failed");
        else { int r=atoi(argv[2]); if(r<2000)r=2000; if(r>4800)r=4800;   // Synapse range 2000-4800 (EC caps ~4700 actual; >4800 gains nothing, tested 2026-06-14)
            int ok=set_fan(fd,r)==0; printf("%s %d (setpoint now %d)\n",ok?"fan manual":"failed",r,get_fan_setpoint(fd)); }
    } else if(!strcmp(argv[1],"kbd")&&argc==3){
        const char*c=argv[2]; int i=-1;
        for(int k=0;k<NKBD;k++) if(!strcmp(c,KBD[k].name)){ i=k; break; }
        if(i<0){ fprintf(stderr,"kbd: white|red|purple|green|off\n"); return 1; }
        if(KBD[i].r<0) printf("%s\n", kbd_off(fd)==0?"kbd off":"failed");
        else printf("%s %s @30%%\n", kbd_color(fd,KBD[i].r,KBD[i].g,KBD[i].b)==0?"kbd":"failed", c);
    } else if(!strcmp(argv[1],"rpm")||!strcmp(argv[1],"watch")){
        printf("live fan rpm @2s (Ctrl-C to stop). NOTE: tach lags ~40-50s after a change.\n");
        for(;;){
            printf("\rfan1=%4d fan2=%4d rpm | setpoint=%-4d perf=%-8s ",
                   get_tach(fd,1),get_tach(fd,2),get_fan_setpoint(fd),modename(get_pmode(fd)));
            fflush(stdout); sleep(2);
        }
    } else if(!strcmp(argv[1],"mode")&&argc==3){
        int m = !strcmp(argv[2],"balanced")?0:!strcmp(argv[2],"gaming")?1:!strcmp(argv[2],"creator")?2:-1;
        if(m<0){ fprintf(stderr,"mode: balanced|gaming|creator\n"); return 1; }
        int r=set_pmode(fd,m);
        printf("%s -> %s  (GPU %s)\n", r==0?"ok":"FAILED", modename(get_pmode(fd)),
               m==2?"eco ~80W, Dynamic Boost OFF":m==1?"boost up to ~175W":"boost ~118W");
    } else if(!strcmp(argv[1],"battery")){
        // EC battery charge limit. SETTER opcode is well-attested from the Synapse capture
        // (0x07/0x12 arg = pct|0x80; 60->bc 65->c1 80->d0 off->41), BUT the readback (0x07/0x8f)
        // does NOT return the percentage -- it gives a status byte (0x02 in Windows, 0x00 here),
        // so a write cannot be auto-verified; verify behaviorally (charging stops near the limit).
        int raw=get_charge_limit(fd);
        if(argc==2||(argc==3&&!strcmp(argv[2],"status"))){
            printf("charge-limit EC byte (0x07/0x8f): %s%d (0x%02x) -- NOTE: this is a status byte, not the %%; readback unconfirmed\n",
                   raw<0?"read FAILED ":"", raw, raw<0?0:raw);
        } else if((argc==3&&!strcmp(argv[2],"off")) || argc==3){
            int off = !strcmp(argv[2],"off");
            int p = off ? -1 : atoi(argv[2]);
            if(!off && (p<50||p>100)){ fprintf(stderr,"battery: <50-100>|off|status  (Synapse range is 50-80)\n"); return 1; }
            int val = off ? 0x41 : (p|0x80);   // off = Synapse's disable byte (0x41 = 65 with bit7 clear -> optimizer off, charge to 100).
                                                // Can't derive it from get_charge_limit (0x8f returns a status byte, not the %), so use a fixed in-range value.
            int r=set_charge_limit(fd,val);
            printf("%s charge limit %s (wrote 0x%02x). Note: the EC readback can't confirm the %%; verify by watching charging stop near the limit.\n",
                   r==0?"ok":"FAILED", off?"disabled":argv[2], val);
        } else { fprintf(stderr,"battery [status | <50-100> | off]\n"); return 1; }
    } else if(!strcmp(argv[1],"powerd")&&argc==3){
        // toggle nvidia-powerd (Dynamic Boost daemon). off => lets dGPU reach D3cold (~0W).
        const char*a=argv[2];
        if(!strcmp(a,"status")){
            printf("nvidia-powerd: %s\n", system("/usr/bin/systemctl is-active --quiet nvidia-powerd")==0?"active":"inactive");
            char ps[48]="?\n",psp[300]; dgpu_path(psp,sizeof psp,"power_state"); FILE*f=fopen(psp,"r");
            if(f){ if(!fgets(ps,sizeof ps,f)) strcpy(ps,"?\n"); fclose(f);}
            else strcpy(ps,"removed (no-dGPU boot mode)\n");
            printf("dGPU power_state: %s", ps);
        } else if(!strcmp(a,"off")||!strcmp(a,"on")){
            // sudo-less via polkit (49-nvidia-powerd.rules); absolute path = don't trust inherited $PATH
            int r=system(!strcmp(a,"off") ? "/usr/bin/systemctl stop nvidia-powerd" : "/usr/bin/systemctl start nvidia-powerd");
            printf("nvidia-powerd %s -> %s\n", a, r==0?"ok":"failed (polkit rule missing?)");
        } else { fprintf(stderr,"powerd: on|off|status\n"); return 1; }
        } else if(!strcmp(argv[1],"fancurve")||!strcmp(argv[1],"autofan")){
        // temp-driven fan loop: CPU + dGPU temp -> rpm via EMA-smoothed curves; Ctrl-C restores AUTO.
        return fan_curve(fd);
    } else if(!strcmp(argv[1],"reclaim")){
        printf("restarting KWin (brief flicker) to release the dGPU...\n");
        int r=reclaim_dgpu();
        printf("%s\n", r==0?"kwin --replace launched":"failed");
    } else if(!strcmp(argv[1],"power")&&argc==3){
        // max: enable nvidia-powerd (Dynamic Boost) + raise TDP to card max.
        // save: disable powerd + reset TDP to driver default.
        // status: report powerd state + current/max/default power limits.
        const char*a=argv[2];
        if(!strcmp(a,"status")){
            printf("nvidia-powerd: %s\n", system("/usr/bin/systemctl is-active --quiet nvidia-powerd")==0?"active (Dynamic Boost ON)":"inactive");
            // GPU Ceiling Power Limit (current), max, and default via -q -d POWER
            FILE*f=popen("/usr/bin/nvidia-smi -q -d POWER 2>/dev/null","r");
            if(f){ char line[128];
                while(fgets(line,sizeof line,f)){
                    if(strstr(line," N/A")) continue; // skip unset base-power fields
                    if(strstr(line,"Current Power Limit")||strstr(line,"Max Power Limit")||strstr(line,"Default Power Limit")||strstr(line,"Average Power Draw"))
                        printf(" %s",line);
                } pclose(f); }
        } else if(!strcmp(a,"max")||!strcmp(a,"save")){
            // sudo-less via polkit (49-nvidia-powerd.rules). Both just toggle nvidia-powerd; the
            // dGPU ceiling is owned by powerd (max=up to 175W) or the driver default (save) which
            // re-applies on the dGPU's next D3cold sleep/wake. (No nvidia-smi -pl: needs root + flaky on laptop GPUs.)
            int rc;
            if(!strcmp(a,"max")){
                rc=system("/usr/bin/systemctl start nvidia-powerd");
                printf("power max: nvidia-powerd (Dynamic Boost) started -> %s\n",rc==0?"ok":"FAILED (polkit rule missing?)");
                printf("  powerd boosts the dGPU ceiling up to 175 W automatically under load.\n");
            } else {
                rc=system("/usr/bin/systemctl stop nvidia-powerd");
                printf("power save: nvidia-powerd stopped -> %s\n",rc==0?"ok":"FAILED (polkit rule missing?)");
                printf("  dGPU ceiling returns to the driver default on its next D3cold sleep/wake.\n");
            }
        } else { fprintf(stderr,"power: max|save|status\n"); return 1; }
    } else {
        printf("usage: razerctl [get | epp [0-255] | rpm | mode <balanced|gaming|creator> | battery [status|<50-100>|off] | fan <auto|RPM> | fancurve | kbd <white|red|purple|green|off> | powerd <on|off|status> | power <max|save|status> | reclaim]   (no args = TUI)\n");
    }
    close(fd); return 0;
}
