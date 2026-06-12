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
#include <linux/hidraw.h>
#define L 91                    // Razer HID feature-report length, in bytes
#define DGPU_PCI "0000:01:00.0"  // NVIDIA dGPU PCI address (sysfs reads + reclaim)
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
static int set_pmode(int fd,int mode){ // Synapse layout (USB capture 2026-06-12): [1,zone,mode,fanflag] on BOTH zones
    if(mode<0||mode==3||mode>4) return -1;          // 0=balanced 1=gaming 2=creator 4=custom
    int sp=get_fan_setpoint(fd); unsigned char flag = sp>0?1:0;
    unsigned char z1[4]={1,1,(unsigned char)mode,flag}, z2[4]={1,2,(unsigned char)mode,flag};
    int ok=0; ok|=snd(fd,0x0d,0x02,0x04,z1,4)!=0x02; ok|=snd(fd,0x0d,0x02,0x04,z2,4)!=0x02;
    if(ok) return -1;
    return get_pmode(fd)==mode?0:-2;
}
// ---- Custom-mode boost + battery charge limit (opcodes from Synapse USB capture 2026-06-12) ----
static int get_boost(int fd,int target){ // target 1=CPU 2=GPU; CPU 0..3=low/medium/high/boost, GPU 0..2
    unsigned char b[L],in[L],a[3]={1,(unsigned char)target,0};
    build(b,0x0d,0x87,0x03,a,3); if(xfer(fd,b,in)!=0x02) return -1; return in[11]; }
static int set_boost(int fd,int target,int lvl){ // only meaningful in Custom mode (mode 4)
    unsigned char a[3]={1,(unsigned char)target,(unsigned char)lvl};
    return snd(fd,0x0d,0x07,0x03,a,3)==0x02?0:-1; }
static int get_charge_limit(int fd){ // raw byte: pct|0x80 when enabled, plain pct when disabled; <0 = error
    unsigned char b[L],in[L],a[1]={0};
    build(b,0x07,0x8f,0x01,a,1); if(xfer(fd,b,in)!=0x02) return -1; return in[9]; }
static int set_charge_limit(int fd,int raw){ // raw = pct|0x80 to enable, pct (bit7 clear) to disable-keep-value
    unsigned char a[1]={(unsigned char)raw};  // NB: Synapse follows every write with 0x07/0x0f arg 0x02; that frame
    return snd(fd,0x07,0x12,0x01,a,1)==0x02?0:-1; } // also fires on the max-fan toggle (ambiguous) -> not replicated
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
static const char* modename(int m){return m==0?"Balanced":m==1?"Gaming":m==2?"Creator":m==4?"Custom":"?";}
// Restart KWin so it drops the dGPU after undocking -> dGPU returns to D3cold.
// Runs as the logged-in user (needs the Wayland session env); brief screen flicker.
static int reclaim_dgpu(void){
    return system("setsid sh -c 'KWIN_DRM_DEVICES=/dev/dri/igpu kwin_wayland --replace >/dev/null 2>&1 &'");
}
// ---------- TUI ----------
static struct termios orig;
static void raw_on(){ tcgetattr(0,&orig); struct termios t=orig; t.c_lflag&=~(ICANON|ECHO);
    t.c_cc[VMIN]=1; t.c_cc[VTIME]=0; tcsetattr(0,TCSANOW,&t); printf("\033[?25l"); }
static void raw_off(){ tcsetattr(0,TCSANOW,&orig); printf("\033[?25h\033[0m\n"); }
static void rds(const char*p,char*o,int n){ o[0]=0; FILE*f=fopen(p,"r"); if(!f)return;
    if(fgets(o,n,f)){ int l=strlen(o); if(l&&o[l-1]==0x0a)o[l-1]=0; } fclose(f); }
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
#define FAN_RPM_MAX    4800   // Synapse's rated ceiling (USB capture 2026-06-12; was 5300, never actually reached)
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
static int dgpu_awake(void){ char s[16]; rds("/sys/bus/pci/devices/" DGPU_PCI "/power_state",s,sizeof s); return !strcmp(s,"D0"); }
// dGPU temp in C, or -1 if the dGPU is asleep/unreadable. Only shells out to nvidia-smi when
// the GPU is ALREADY D0 -- an idle dGPU stays in D3cold and we must never wake it just to poll.
static int gpu_temp(void){
    if(!dgpu_awake()) return -1;
    FILE*f=popen("/usr/bin/nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null","r");
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
static void draw(const char*node,int pm,int manual,int curve,int alarm,int rpm,int sp,int f1,int f2,double bw,int ac,const char*gst,const char*gps,int temp,double pkgw,double corew,double busy,double deep,int meanf,int maxf,int epp,const char*eppname,int mon,const char*kbd,int powerd_on,int cb,int gb,int btgt,const char*msg){
    static const char*BN[4]={"low","medium","high","boost"};
    printf("\033[2J\033[H\033[1;36m  razerctl\033[0m  Blade 16 (1532:02b7) fw1.3  [%s]\n",node);
    printf("  --------------------------------------------\n");
    if(pm==4){
        printf("   Perf mode : \033[1;33m%-9s\033[0m  CPU \033[1;36m%-6s\033[0m  GPU \033[1;36m%-6s\033[0m\n",
               modename(pm), (cb>=0&&cb<4)?BN[cb]:"?", (gb>=0&&gb<3)?BN[gb]:"?");
        printf("   Boost set : editing \033[1;33m%s\033[0m  \033[1;90m1\033[0m=low \033[1;90m2\033[0m=medium \033[1;90m3\033[0m=high%s   \033[1;90m(g: CPU/GPU)\033[0m\n",
               btgt==1?"CPU":"GPU", btgt==1?" \033[1;90m4\033[0m=boost":"");
    } else printf("   Perf mode : \033[1;33m%-9s\033[0m\n",modename(pm));
    printf("   Fan       : \033[1;33m%-7s\033[0m target \033[1;33m%d rpm\033[0m (setpoint %d)%s\n",
           curve?"CURVE":(manual?"MANUAL":"Auto"), (curve||manual)?rpm:0, sp,
           alarm?"  \033[1;31m[ALARM]\033[0m":"");
    if(mon){
    printf("   Fan RPM   : \033[1;32mfan1 %4d   fan2 %4d\033[0m  (live, lags ~50s)\n",f1,f2);
    printf("   Battery   : \033[1;33m%s  %.1f W\033[0m\n", ac?"AC":"BAT", bw);
    { int gone = (gst[0]==0);   // sysfs node absent = GPU removed from PCI bus (no-dGPU boot mode)
      int asleep = gone || (strcmp(gst,"suspended")==0);
      printf("   dGPU      : %s%s (%s)\033[0m\n", asleep?"\033[1;32m":"\033[1;31m",
             gone?"absent":gps, gone?"removed ~0W (no-dGPU boot)":(asleep?"asleep ~0W":"AWAKE drawing")); }
    printf("   DynBoost  : %s\033[0m  (d=toggle; needs sudo razerctl)\n", powerd_on?"\033[1;32mON  (up to 175 W)":"\033[1;31mOFF (80 W cap)");
    printf("   CPU       : \033[1;33m%dC\033[0m  pkg \033[1;33m%.1fW\033[0m  core \033[1;33m%.1fW\033[0m\n", temp, pkgw, corew);
    if(meanf>0) printf("   CPU freq  : \033[1;33mmean %.2f GHz\033[0m   \033[1;32mpeak %.2f GHz\033[0m\n", meanf/1000.0, maxf/1000.0);
    else        printf("   CPU freq  : \033[1;90mn/a\033[0m\n");
    printf("   C-state   : busy \033[1;33m%.0f%%\033[0m  deep(C3) \033[1;32m%.0f%%\033[0m\n", busy, deep);
    } else {
    printf("   \033[1;90m-- monitoring PAUSED (no polling) --\033[0m\n");
    }
    if(epp>=0) printf("   CPU bias  : \033[1;33mEPP %3d\033[0m (%-19s) \033[1;90m0=perf..255=save; TLP resets on AC/DC\033[0m\n", epp, eppname);
    else       printf("   CPU bias  : \033[1;90mEPP n/a (no intel_pstate HWP)\033[0m\n");
    printf("   Kbd light : \033[1;33m%s\033[0m  (30%% when on)\n", kbd);
    printf("  --------------------------------------------\n");
    printf("   m: mode  1-4/g: cpu/gpu boost (Custom)  f: fan  c: curve  +/-: rpm  k: kbd  e: EPP  d: dynboost  b: batt-limit  w: reclaim  p: pause  r/q\n");
    if(msg&&*msg) printf("\n   \033[32m%s\033[0m\n",msg);
    fflush(stdout);
}
// "on" = nvidia-powerd ACTIVE this boot. We toggle via start/stop (runtime, polkit-scoped to the unit),
// NOT enable/disable, so it's intentionally non-persistent: a reboot returns to powerd OFF (D3cold default).
static int powerd_enabled(void){ return system("/usr/bin/systemctl is-active --quiet nvidia-powerd 2>/dev/null")==0; }
static int tui(int fd,const char*node){
    int manual=0,rpm=4000; char msg[160]="";
    int curve=0,alarm=0,capplied=-1; double ec=-1,eg=-1;   // temp-curve toggle: EMA temps + last-applied rpm
    int pm=get_pmode(fd), sp=get_fan_setpoint(fd); manual = sp>0; if(manual) rpm=sp;
    long long pe=-1,ce=-1; double pts=0,cts=0; int ncpu=(int)sysconf(_SC_NPROCESSORS_ONLN); int mon=1;
    int kbi=0; int powerd_on=powerd_enabled(); int eppi=epp_nearest(get_epp(NULL,0)); int batti=0; int btgt=1;
    raw_on();
    for(;;){
        int f1=-1,f2=-1,temp=-1,ac=-1,meanf=-1,maxf=-1; double bw=0,pkgw=-1,corew=-1,busy=-1,deep=-1;
        char gst[32]="?",gps[16]="?";
        if(mon){
            f1=get_tach(fd,1); f2=get_tach(fd,2);
            bw=batt_watts(&ac);
            rds("/sys/bus/pci/devices/" DGPU_PCI "/power/runtime_status",gst,sizeof gst);
            rds("/sys/bus/pci/devices/" DGPU_PCI "/power/../power_state",gps,sizeof gps);
            pkgw=rapl_w("/sys/class/powercap/intel-rapl:0/energy_uj",&pe,&pts);
            corew=rapl_w("/sys/class/powercap/intel-rapl:0:0/energy_uj",&ce,&cts);
            temp=pkg_temp(); busy=cpu_busy(); deep=deep_res(ncpu);
            cpu_freq(ncpu,&meanf,&maxf);
            powerd_on=powerd_enabled();
        }
        if(curve){   // temp-driven: advance the EMA curve and apply with hysteresis (capplied = last write)
            int ct,gt; rpm=curve_step(&ec,&eg,&ct,&gt,&alarm); (void)ct;(void)gt;
            if(capplied<0||alarm||abs(rpm-capplied)>=FAN_STEP){ if(set_fan(fd,rpm)==0) capplied=rpm; }
        } else alarm=0;
        char eppname[24]; int epp=get_epp(eppname,sizeof eppname);
        int cb=pm==4?get_boost(fd,1):-1, gb=pm==4?get_boost(fd,2):-1;   // boost levels only meaningful in Custom
        draw(node,pm,manual,curve,alarm,rpm,sp,f1,f2,bw,ac,gst,gps,temp,pkgw,corew,busy,deep,meanf,maxf,epp,eppname,mon,KBD[kbi].name,powerd_on,cb,gb,btgt,msg);
        fd_set r; FD_ZERO(&r); FD_SET(0,&r); struct timeval tv={2,0};
        if(select(1,&r,0,0, (mon||curve)?&tv:NULL)<=0) continue;  // paused & no curve: block until key
        msg[0]=0; int ch=getchar();
        if(ch=='q'){ if(curve) set_fan_auto(fd); break; }   // curve owns the fan -> hand back to AUTO on exit
        else if(ch=='r'){ pm=get_pmode(fd); sp=get_fan_setpoint(fd); powerd_on=powerd_enabled(); snprintf(msg,sizeof msg,"refreshed"); }
        else if(ch=='p'){ mon=!mon; pe=ce=-1; snprintf(msg,sizeof msg, mon?"monitor ON":"monitor PAUSED"); }
        else if(ch=='w'){ reclaim_dgpu(); snprintf(msg,sizeof msg,"KWin restarted (reclaim dGPU)"); }
        else if(ch=='d'){
            // toggle Dynamic Boost (nvidia-powerd); sudo-less via polkit (49-nvidia-powerd.rules)
            int r=system(powerd_on
                ? "/usr/bin/systemctl stop nvidia-powerd 2>/dev/null"
                : "/usr/bin/systemctl start nvidia-powerd 2>/dev/null");
            powerd_on=powerd_enabled();
            if(r!=0) snprintf(msg,sizeof msg,"boost toggle failed (polkit rule missing?)");
            else snprintf(msg,sizeof msg,"DynBoost -> %s",powerd_on?"ON (up to 175 W)":"OFF (80 W cap)");
        }
        else if(ch=='k'){ kbi=(kbi+1)%NKBD;
            if(KBD[kbi].r<0) kbd_off(fd); else kbd_color(fd,KBD[kbi].r,KBD[kbi].g,KBD[kbi].b);
            snprintf(msg,sizeof msg,"kbd -> %s",KBD[kbi].name); }
        else if(ch=='e'){ eppi=(eppi+1)%NEPP; int v=EPP_PRESET[eppi];
            if(set_epp(v)==0) snprintf(msg,sizeof msg,"EPP -> %d  (TLP resets it on the next AC/DC switch)",v);
            else snprintf(msg,sizeof msg,"EPP needs sudo: run as  sudo razerctl"); }
        else if(ch=='m'){ int w=(pm==0?1:pm==1?2:pm==2?4:0); set_pmode(fd,w); pm=get_pmode(fd);  // cycle ...creator->custom->balanced
            if(pm==4){ int c=get_boost(fd,1),g=get_boost(fd,2);
                snprintf(msg,sizeof msg,"mode -> Custom (cpu boost %d, gpu boost %d; set via CLI: razerctl boost)",c,g); }
            else snprintf(msg,sizeof msg, pm==w?"mode -> %s":"mode FAILED (%s)", modename(pm)); }
        else if(ch=='f'){ if(curve){curve=0;alarm=0;}   // leaving curve -> fall through to manual at the last target
            if(manual){ manual=0; set_fan_auto(fd); snprintf(msg,sizeof msg,"fan AUTO"); }
            else { manual=1; set_fan(fd,rpm); snprintf(msg,sizeof msg,"fan MANUAL %d",rpm); } sp=get_fan_setpoint(fd); }
        else if(ch=='c'){ curve=!curve;   // toggle the temp-driven curve
            if(curve){ manual=0; ec=eg=-1; capplied=-1; snprintf(msg,sizeof msg,"fan CURVE on (CPU %d->%dC  GPU %d->%dC; f or q restores AUTO)",CPU_T_LO,CPU_T_HI,GPU_T_LO,GPU_T_HI); }
            else { set_fan_auto(fd); sp=get_fan_setpoint(fd); alarm=0; snprintf(msg,sizeof msg,"fan CURVE off -> AUTO"); } }
        else if(ch=='+'||ch=='='){ if(manual){ rpm+=500; if(rpm>4800)rpm=4800; set_fan(fd,rpm); sp=get_fan_setpoint(fd); snprintf(msg,sizeof msg,"rpm %d",rpm);} }
        else if(ch=='-'||ch=='_'){ if(manual){ rpm-=500; if(rpm<2000)rpm=2000; set_fan(fd,rpm); sp=get_fan_setpoint(fd); snprintf(msg,sizeof msg,"rpm %d",rpm);} }
        else if(ch=='b'){ // cycle battery charge limit: off -> 60 -> 70 -> 80 -> off
            static const int BP[NBATT]={0,60,70,80}; batti=(batti+1)%NBATT; int p=BP[batti];
            int raw=get_charge_limit(fd);
            int val = p? (p|0x80) : ((raw<0?0:raw)&0x7f);  // 0 = disable, keep stored value
            int r=set_charge_limit(fd,val);
            if(p) snprintf(msg,sizeof msg,"charge limit -> %d%% (wrote 0x%02x) - verify by watching charging stop near it",p,val);
            else  snprintf(msg,sizeof msg,"charge limit OFF (wrote 0x%02x)",val);
            if(r!=0) snprintf(msg,sizeof msg,"charge-limit write FAILED"); }
        else if(ch=='g'){ btgt = btgt==1?2:1;   // switch which engine the 1-4 keys edit
            snprintf(msg,sizeof msg,"editing %s boost (1=low 2=medium 3=high%s)",btgt==1?"CPU":"GPU",btgt==1?" 4=boost":""); }
        else if(ch>='1'&&ch<='4'){ // set the selected engine's power sub-level directly (Custom mode only)
            static const char*BN[4]={"low","medium","high","boost"};
            int max = btgt==1?3:2, lvl=ch-'1';
            if(lvl>max){ snprintf(msg,sizeof msg,"GPU has no 'boost' level (max=high); press g for CPU"); }
            else { int r=set_boost(fd,btgt,lvl); int now=get_boost(fd,btgt);
                if(r!=0) snprintf(msg,sizeof msg,"%s boost write FAILED",btgt==1?"CPU":"GPU");
                else if(pm!=4) snprintf(msg,sizeof msg,"%s boost -> %s (stored; active only in Custom - press m)",btgt==1?"CPU":"GPU",BN[now]);
                else snprintf(msg,sizeof msg,"%s boost -> %s",btgt==1?"CPU":"GPU",BN[now]); } }
    }
    raw_off(); return 0;
}
int main(int argc,char**argv){
    char node[64]="?"; int fd=open_dev(node);
    if(fd<0){ fprintf(stderr,"razerctl: no responding 1532:02b7 hidraw (root? udev rule?)\n"); return 2; }
    if(argc==1) return tui(fd,node);
    if(!strcmp(argv[1],"get")){
        char en[24]; int ev=get_epp(en,sizeof en);
        printf("node=%s perf=%s fan-setpoint=%d rpm (0=auto) epp=%d (%s)\n",node,modename(get_pmode(fd)),get_fan_setpoint(fd),ev,en);
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
        else { int r=atoi(argv[2]); if(r<2000)r=2000; if(r>4800)r=4800;   // Synapse range 2000-4800
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
        int m = !strcmp(argv[2],"balanced")?0:!strcmp(argv[2],"gaming")?1:!strcmp(argv[2],"creator")?2:!strcmp(argv[2],"custom")?4:-1;
        if(m<0){ fprintf(stderr,"mode: balanced|gaming|creator|custom\n"); return 1; }
        int r=set_pmode(fd,m); printf("%s -> %s\n", r==0?"ok":"FAILED", modename(get_pmode(fd)));
        if(m==4&&r==0) printf("  custom mode active: set sub-levels with  razerctl boost cpu|gpu <level>\n");
    } else if(!strcmp(argv[1],"boost")){
        // CPU/GPU power sub-levels, Custom mode only. CPU: 0=low 1=medium 2=high 3=boost; GPU: 0=low 1=medium 2=high.
        static const char*BN[4]={"low","medium","high","boost"};
        if(argc==2){
            int c=get_boost(fd,1), g=get_boost(fd,2), pm=get_pmode(fd);
            printf("cpu boost: %d (%s)   gpu boost: %d (%s)   [perf mode: %s]\n",
                   c, (c>=0&&c<4)?BN[c]:"?", g, (g>=0&&g<3)?BN[g]:"?", modename(pm));
            if(pm!=4) printf("note: boost levels only take effect in Custom mode (razerctl mode custom)\n");
        } else if(argc==4){
            int tgt = !strcmp(argv[2],"cpu")?1:!strcmp(argv[2],"gpu")?2:-1;
            if(tgt<0){ fprintf(stderr,"boost: cpu|gpu <low|medium|high|boost|0-3>\n"); return 1; }
            int max = tgt==1?3:2, lvl=-1;
            for(int k=0;k<=max;k++) if(!strcmp(argv[3],BN[k])) lvl=k;
            if(lvl<0&&argv[3][0]>='0'&&argv[3][0]<='9') lvl=atoi(argv[3]);
            if(lvl<0||lvl>max){ fprintf(stderr,"boost %s: low|medium|high%s (0-%d)\n",argv[2],tgt==1?"|boost":"",max); return 1; }
            if(get_pmode(fd)!=4) printf("note: not in Custom mode - value is stored but only applies in Custom\n");
            int r=set_boost(fd,tgt,lvl); int now=get_boost(fd,tgt);
            printf("%s %s boost -> %d (%s), readback %d\n", r==0?"ok":"FAILED", argv[2], lvl, BN[lvl], now);
        } else { fprintf(stderr,"boost            (show)\nboost cpu|gpu <low|medium|high|boost|0-3>\n"); return 1; }
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
            int val = off ? ((raw<0?0:raw)&0x7f) : (p|0x80);   // off = clear bit7, keep stored value
            int r=set_charge_limit(fd,val);
            printf("%s charge limit %s (wrote 0x%02x). Note: the EC readback can't confirm the %%; verify by watching charging stop near the limit.\n",
                   r==0?"ok":"FAILED", off?"disabled":argv[2], val);
        } else { fprintf(stderr,"battery [status | <50-100> | off]\n"); return 1; }
    } else if(!strcmp(argv[1],"powerd")&&argc==3){
        // toggle nvidia-powerd (Dynamic Boost daemon). off => lets dGPU reach D3cold (~0W).
        const char*a=argv[2];
        if(!strcmp(a,"status")){
            printf("nvidia-powerd: %s\n", system("/usr/bin/systemctl is-active --quiet nvidia-powerd")==0?"active":"inactive");
            char ps[48]="?\n"; FILE*f=fopen("/sys/bus/pci/devices/" DGPU_PCI "/power_state","r");
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
        printf("usage: razerctl [get | epp [0-255] | rpm | mode <balanced|gaming|creator|custom> | boost [cpu|gpu <level>] | battery [status|<50-100>|off] | fan <auto|RPM> | fancurve | kbd <white|red|purple|green|off> | powerd <on|off|status> | power <max|save|status> | reclaim]   (no args = TUI)\n");
    }
    close(fd); return 0;
}
