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
#include <time.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#define L 91
static unsigned char TID=0x1f;
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
    unsigned char z1e[4]={0,1,(unsigned char)m,1}, z1r[3]={0,1,(unsigned char)sp};
    unsigned char z2e[4]={0,2,(unsigned char)m,1}, z2r[3]={0,2,(unsigned char)sp};
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
static int set_pmode(int fd,int mode){ // verified layout: [0,1,mode,fanflag]
    if(mode<0||mode>2) return -1;
    int sp=get_fan_setpoint(fd); unsigned char flag = sp>0?1:0;
    unsigned char a[4]={0,1,(unsigned char)mode,flag};
    if(snd(fd,0x0d,0x02,0x04,a,4)!=0x02) return -1;
    return get_pmode(fd)==mode?0:-2;
}
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
static const char* modename(int m){return m==0?"Balanced":m==1?"Gaming":m==2?"Creator":m==4?"Custom":"?";}
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
    if(de<0) de+=262143328850LL;            /* energy counter wrap */
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
static void draw(const char*node,int pm,int manual,int rpm,int sp,int f1,int f2,double bw,int ac,const char*gst,const char*gps,int temp,double pkgw,double corew,double busy,double deep,int mon,const char*kbd,const char*msg){
    printf("\033[2J\033[H\033[1;36m  razerctl\033[0m  Blade 16 (1532:02b7) fw1.3  [%s]\n",node);
    printf("  --------------------------------------------\n");
    printf("   Perf mode : \033[1;33m%-9s\033[0m\n",modename(pm));
    printf("   Fan       : \033[1;33m%-7s\033[0m target \033[1;33m%d rpm\033[0m (setpoint %d)\n",
           manual?"MANUAL":"Auto", manual?rpm:0, sp);
    if(mon){
    printf("   Fan RPM   : \033[1;32mfan1 %4d   fan2 %4d\033[0m  (live, lags ~50s)\n",f1,f2);
    printf("   Battery   : \033[1;33m%s  %.1f W\033[0m\n", ac?"AC":"BAT", bw);
    { int asleep = (strcmp(gst,"suspended")==0);
      printf("   dGPU      : %s%s (%s)\033[0m\n", asleep?"\033[1;32m":"\033[1;31m", gps, asleep?"asleep ~0W":"AWAKE drawing"); }
    printf("   CPU       : \033[1;33m%dC\033[0m  pkg \033[1;33m%.1fW\033[0m  core \033[1;33m%.1fW\033[0m\n", temp, pkgw, corew);
    printf("   C-state   : busy \033[1;33m%.0f%%\033[0m  deep(C3) \033[1;32m%.0f%%\033[0m\n", busy, deep);
    } else {
    printf("   \033[1;90m-- monitoring PAUSED (no polling) --\033[0m\n");
    }
    printf("   Kbd light : \033[1;33m%s\033[0m  (30%% when on)\n", kbd);
    printf("  --------------------------------------------\n");
    printf("   m: mode  f: fan  +/-: rpm  k: kbd color  p: pause mon  r: refresh  q: quit\n");
    if(msg&&*msg) printf("\n   \033[32m%s\033[0m\n",msg);
    fflush(stdout);
}
static int tui(int fd,const char*node){
    int manual=0,rpm=4000; char msg[160]="";
    int pm=get_pmode(fd), sp=get_fan_setpoint(fd); manual = sp>0; if(manual) rpm=sp;
    long long pe=-1,ce=-1; double pts=0,cts=0; int ncpu=(int)sysconf(_SC_NPROCESSORS_ONLN); int mon=1;
    const char*kbdn[]={"off","white","red","purple","green"}; int kbi=0;
    raw_on();
    for(;;){
        int f1=-1,f2=-1,temp=-1,ac=-1; double bw=0,pkgw=-1,corew=-1,busy=-1,deep=-1;
        char gst[32]="?",gps[16]="?";
        if(mon){
            f1=get_tach(fd,1); f2=get_tach(fd,2);
            bw=batt_watts(&ac);
            rds("/sys/bus/pci/devices/0000:01:00.0/power/runtime_status",gst,sizeof gst);
            rds("/sys/bus/pci/devices/0000:01:00.0/power/../power_state",gps,sizeof gps);
            pkgw=rapl_w("/sys/class/powercap/intel-rapl:0/energy_uj",&pe,&pts);
            corew=rapl_w("/sys/class/powercap/intel-rapl:0:0/energy_uj",&ce,&cts);
            temp=pkg_temp(); busy=cpu_busy(); deep=deep_res(ncpu);
        }
        draw(node,pm,manual,rpm,sp,f1,f2,bw,ac,gst,gps,temp,pkgw,corew,busy,deep,mon,kbdn[kbi],msg);
        fd_set r; FD_ZERO(&r); FD_SET(0,&r); struct timeval tv={2,0};
        if(select(1,&r,0,0, mon?&tv:NULL)<=0) continue;  // paused: block until key (no polling)
        msg[0]=0; int ch=getchar();
        if(ch=='q') break;
        else if(ch=='r'){ pm=get_pmode(fd); sp=get_fan_setpoint(fd); snprintf(msg,sizeof msg,"refreshed"); }
        else if(ch=='p'){ mon=!mon; pe=ce=-1; snprintf(msg,sizeof msg, mon?"monitor ON":"monitor PAUSED"); }
        else if(ch=='k'){ kbi=(kbi+1)%5;
            if(kbi==0) kbd_off(fd);
            else { int r=0,g=0,b=0; if(kbi==1){r=g=b=255;} else if(kbi==2){r=255;} else if(kbi==3){r=128;b=128;} else {g=255;} kbd_color(fd,r,g,b); }
            snprintf(msg,sizeof msg,"kbd -> %s",kbdn[kbi]); }
        else if(ch=='m'){ int w=(pm==0?1:pm==1?2:0); set_pmode(fd,w); pm=get_pmode(fd);
            snprintf(msg,sizeof msg, pm==w?"mode -> %s":"mode FAILED (%s)", modename(pm)); }
        else if(ch=='f'){ if(manual){ manual=0; set_fan_auto(fd); snprintf(msg,sizeof msg,"fan AUTO"); }
            else { manual=1; set_fan(fd,rpm); snprintf(msg,sizeof msg,"fan MANUAL %d",rpm); } sp=get_fan_setpoint(fd); }
        else if(ch=='+'||ch=='='){ if(manual){ rpm+=500; if(rpm>5300)rpm=5300; set_fan(fd,rpm); sp=get_fan_setpoint(fd); snprintf(msg,sizeof msg,"rpm %d",rpm);} }
        else if(ch=='-'||ch=='_'){ if(manual){ rpm-=500; if(rpm<2000)rpm=2000; set_fan(fd,rpm); sp=get_fan_setpoint(fd); snprintf(msg,sizeof msg,"rpm %d",rpm);} }
    }
    raw_off(); return 0;
}
int main(int argc,char**argv){
    char node[64]="?"; int fd=open_dev(node);
    if(fd<0){ fprintf(stderr,"razerctl: no responding 1532:02b7 hidraw (root? udev rule?)\n"); return 2; }
    if(argc==1) return tui(fd,node);
    if(!strcmp(argv[1],"get")){
        printf("node=%s perf=%s fan-setpoint=%d rpm (0=auto)\n",node,modename(get_pmode(fd)),get_fan_setpoint(fd));
    } else if(!strcmp(argv[1],"fan")&&argc==3){
        if(!strcmp(argv[2],"auto")) printf("%s\n",set_fan_auto(fd)==0?"fan auto":"failed");
        else { int r=atoi(argv[2]); if(r<2000)r=2000; if(r>5300)r=5300;
            int ok=set_fan(fd,r)==0; printf("%s %d (setpoint now %d)\n",ok?"fan manual":"failed",r,get_fan_setpoint(fd)); }
    } else if(!strcmp(argv[1],"kbd")&&argc==3){
        const char*c=argv[2]; int r=0,g=0,b=0,col=1;
        if(!strcmp(c,"off")){ printf("%s\n", kbd_off(fd)==0?"kbd off":"failed"); col=0; }
        else if(!strcmp(c,"white")){ r=g=b=255; }
        else if(!strcmp(c,"red")){ r=255; }
        else if(!strcmp(c,"purple")){ r=128; b=128; }
        else if(!strcmp(c,"green")){ g=255; }
        else { fprintf(stderr,"kbd: white|red|purple|green|off\n"); return 1; }
        if(col) printf("%s %s @30%%\n", kbd_color(fd,r,g,b)==0?"kbd":"failed", c);
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
        int r=set_pmode(fd,m); printf("%s -> %s\n", r==0?"ok":"FAILED", modename(get_pmode(fd)));
    } else if(!strcmp(argv[1],"powerd")&&argc==3){
        // toggle nvidia-powerd (Dynamic Boost daemon). off => lets dGPU reach D3cold (~0W).
        const char*a=argv[2];
        if(!strcmp(a,"status")){
            printf("nvidia-powerd: %s\n", system("systemctl is-active --quiet nvidia-powerd")==0?"active":"inactive");
            char ps[16]="?\n"; FILE*f=fopen("/sys/bus/pci/devices/0000:01:00.0/power_state","r");
            if(f){ if(!fgets(ps,sizeof ps,f)) ps[0]=0; fclose(f);}
            printf("dGPU power_state: %s", ps);
        } else if(!strcmp(a,"off")||!strcmp(a,"on")){
            if(geteuid()!=0){ fprintf(stderr,"powerd %s needs root: sudo razerctl powerd %s\n",a,a); return 1; }
            int r=system(!strcmp(a,"off") ? "systemctl disable --now nvidia-powerd" : "systemctl enable --now nvidia-powerd");
            printf("nvidia-powerd %s -> %s\n", a, r==0?"ok":"failed");
        } else { fprintf(stderr,"powerd: on|off|status\n"); return 1; }
    } else {
        printf("usage: razerctl [get | rpm | mode <balanced|gaming|creator> | fan <auto|RPM> | kbd <white|red|purple|green|off> | powerd <on|off|status>]   (no args = TUI)\n");
    }
    close(fd); return 0;
}
