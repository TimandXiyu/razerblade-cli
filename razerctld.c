// SPDX-License-Identifier: GPL-2.0-only
// razerctld - background owner of the Razer Blade 16 EC (1532:02b7) over hidraw.
// Persists perf mode, fan mode, keyboard backlight, and battery charge limit
// across boots, and keeps them consistent with zero client attached. All the
// live-telemetry heavy lifting (fan tach, RAPL wattage, dGPU polling, CPU
// busy%, EPP, dGPU undervolt) stays in the `razerctl` client -- none of that
// touches hidraw, so it never needs the daemon and never runs unless a client
// is actively asking (request/response only, no daemon-side polling).
//
// Protocol: one line in, one line out, per accepted connection (see razer_ipc.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <linux/hidraw.h>
#include "razer_ipc.h"
#define L 91
static unsigned char TID=0x1f;

// ---------------- hidraw protocol core (verbatim from razerctl.c) ----------------
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
static int get_pmode(int fd){ unsigned char b[L],in[L],a[2]={0,1};
    build(b,0x0d,0x82,0x04,a,2); if(xfer(fd,b,in)!=0x02) return -1; return in[11]; }
static int get_fan_setpoint(int fd){ unsigned char b[L],in[L],a[2]={0,1};
    build(b,0x0d,0x81,0x04,a,2); if(xfer(fd,b,in)!=0x02) return -1; return in[11]*100; }
static int get_tach(int fd,int zone){ unsigned char b[L],in[L],a[2]={0,(unsigned char)zone};
    build(b,0x0d,0x88,0x04,a,2); if(xfer(fd,b,in)!=0x02) return -1; return in[11]*100;
}
static int set_fan(int fd,int rpm){
    int sp=rpm/100; int m=get_pmode(fd); if(m<0||m>=4) m=0;
    unsigned char z1e[4]={0,1,(unsigned char)m,1}, z1r[3]={1,1,(unsigned char)sp};
    unsigned char z2e[4]={0,2,(unsigned char)m,1}, z2r[3]={1,2,(unsigned char)sp};
    snd(fd,0x0d,0x82,0x04,(unsigned char[]){0,1,0,0},4);
    int ok=0;
    ok|=snd(fd,0x0d,0x02,0x04,z1e,4)!=0x02;
    ok|=snd(fd,0x0d,0x01,0x03,z1r,3)!=0x02;
    ok|=snd(fd,0x0d,0x02,0x04,z2e,4)!=0x02;
    ok|=snd(fd,0x0d,0x01,0x03,z2r,3)!=0x02;
    return ok?-1:0;
}
static int set_fan_auto(int fd){
    int m=get_pmode(fd); if(m<0||m>=4) m=0;
    unsigned char z1[4]={0,1,(unsigned char)m,0}, z2[4]={0,2,(unsigned char)m,0};
    int ok=0; ok|=snd(fd,0x0d,0x02,0x04,z1,4)!=0x02; ok|=snd(fd,0x0d,0x02,0x04,z2,4)!=0x02;
    return ok?-1:0;
}
static int set_pmode(int fd,int mode,int force_manual){
    if(mode<0||mode>2) return -1;
    unsigned char flag = force_manual?1:0;
    unsigned char z1[4]={1,1,(unsigned char)mode,flag}, z2[4]={1,2,(unsigned char)mode,flag};
    int ok=0; ok|=snd(fd,0x0d,0x02,0x04,z1,4)!=0x02; ok|=snd(fd,0x0d,0x02,0x04,z2,4)!=0x02;
    if(ok) return -1;
    if(get_pmode(fd)!=mode) return -2;
    system(mode==2 ? "/usr/bin/systemctl stop nvidia-powerd 2>/dev/null"
                   : "/usr/bin/systemctl start nvidia-powerd 2>/dev/null");
    system(mode==2 ? "echo powersupersave > /sys/module/pcie_aspm/parameters/policy 2>/dev/null"
                   : "echo default > /sys/module/pcie_aspm/parameters/policy 2>/dev/null");
    return 0;
}
static int get_charge_limit(int fd){ unsigned char b[L],in[L],a[1]={0};
    build(b,0x07,0x8f,0x01,a,1); if(xfer(fd,b,in)!=0x02) return -1; return in[9]; }
static int set_charge_limit(int fd,int raw){
    unsigned char a[1]={(unsigned char)raw};
    if(snd(fd,0x07,0x12,0x01,a,1)!=0x02) return -1;
    (void)get_charge_limit(fd);
    unsigned char c[1]={0x02};
    return snd(fd,0x07,0x0f,0x01,c,1)==0x02?0:-1;
}
static int kbd_off(int fd){ unsigned char a[3]={0x01,0x05,0x00}; return snd(fd,0x03,0x03,0x03,a,3)==0x02?0:-1; }
static int kbd_color(int fd,int r,int g,int b){
    for(int row=0;row<6;row++){
        unsigned char a[52]; memset(a,0,sizeof a);
        a[0]=0xff; a[1]=(unsigned char)row; a[3]=0x0f;
        for(int k=0;k<15;k++){ a[7+k*3]=(unsigned char)r; a[7+k*3+1]=(unsigned char)g; a[7+k*3+2]=(unsigned char)b; }
        if(snd(fd,0x03,0x0b,0x34,a,52)!=0x02) return -1;
    }
    unsigned char cm[2]={0x05,0x00}; snd(fd,0x03,0x0a,0x02,cm,2);
    unsigned char br[3]={0x01,0x05,76}; snd(fd,0x03,0x03,0x03,br,3);
    return 0;
}
static const struct { const char*name; int r,g,b; } KBD[] = {
    {"off",-1,-1,-1}, {"white",255,255,255}, {"red",255,0,0}, {"purple",128,0,128}, {"green",0,255,0}
};
#define NKBD ((int)(sizeof KBD / sizeof KBD[0]))

// ---------------- temp-driven fan curve (moved here verbatim: it's an ongoing
// control loop, not "monitoring" -- it has to run whenever fan mode is Curve,
// TUI attached or not, or the fan wouldn't respond to load) ----------------
static void rds(const char*p,char*o,int n){ o[0]=0; FILE*f=fopen(p,"r"); if(!f)return;
    if(fgets(o,n,f)){ int l=strlen(o); if(l&&o[l-1]==0x0a)o[l-1]=0; } fclose(f); }
static long long rdll(const char*p){FILE*f=fopen(p,"r");if(!f)return -1;long long v=-1;if(fscanf(f,"%lld",&v)!=1)v=-1;fclose(f);return v;}
static double mono(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static int pkg_temp(void){
    static char path[160]; static int done=0;
    if(!done){ done=1; path[0]=0;
        for(int h=0;h<24&&!path[0];h++){ char np[96]; snprintf(np,sizeof np,"/sys/class/hwmon/hwmon%d/name",h);
            char nm[32]; rds(np,nm,sizeof nm); if(strcmp(nm,"coretemp")) continue;
            for(int t=1;t<48;t++){ char lp[160]; snprintf(lp,sizeof lp,"/sys/class/hwmon/hwmon%d/temp%d_label",h,t);
                char lab[40]; rds(lp,lab,sizeof lab); if(strstr(lab,"Package")){ snprintf(path,sizeof path,"/sys/class/hwmon/hwmon%d/temp%d_input",h,t); break; } } } }
    if(!path[0])return -1; long long v=rdll(path); return v<0?-1:(int)(v/1000);
}
static const char* dgpu_pci(void){
    // razerctl.c's client-side copy does full PCI-bus auto-discovery (the dGPU undervolt
    // page needs it to be robust across boots); the daemon only needs it for the fan curve's
    // GPU-temp read, so the common slot is a fine fallback -- nvidia-smi is address-independent
    // anyway (gpu_temp() below doesn't even use this path, kept for symmetry/future use).
    return "0000:01:00.0";
}
static void dgpu_path(char*buf,int n,const char*suffix){ snprintf(buf,n,"/sys/bus/pci/devices/%s/%s",dgpu_pci(),suffix); }
static int dgpu_awake(void){ char s[16],p[300]; dgpu_path(p,sizeof p,"power_state"); rds(p,s,sizeof s); return !strcmp(s,"D0"); }
static int gpu_temp(void){
    if(!dgpu_awake()) return -1;
    FILE*f=popen("/usr/bin/timeout 2 /usr/bin/nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null","r");
    if(!f) return -1; int t=-1; if(fscanf(f,"%d",&t)!=1) t=-1; pclose(f); return t;
}
#define FAN_RPM_IDLE   2200
#define FAN_RPM_ENGAGE 2500
#define FAN_RPM_MAX    4800
#define PRE_T_BAND     10
#define CPU_T_LO 65
#define CPU_T_HI 90
#define GPU_T_LO 70
#define GPU_T_HI 82
#define EMA_UP   0.50
#define EMA_DOWN 0.25
#define FAN_STEP 150
#define FAN_LOOP_S 2
static int curve_rpm(double t,double lo,double hi){
    if(t<=lo-PRE_T_BAND) return 0;
    if(t<lo) return FAN_RPM_IDLE + (int)((FAN_RPM_ENGAGE-FAN_RPM_IDLE)*(t-(lo-PRE_T_BAND))/PRE_T_BAND+0.5);
    if(t>=hi) return FAN_RPM_MAX;
    return FAN_RPM_ENGAGE + (int)((FAN_RPM_MAX-FAN_RPM_ENGAGE)*(t-lo)/(hi-lo)+0.5);
}
static int curve_step(double*ec,double*eg,int*alm){
    int ct=pkg_temp(), gt=gpu_temp();
    if(ct>=0){ if(*ec<0)*ec=ct; else *ec += ((ct>*ec)?EMA_UP:EMA_DOWN)*(ct-*ec); }
    if(gt>=0){ if(*eg<0)*eg=gt; else *eg += ((gt>*eg)?EMA_UP:EMA_DOWN)*(gt-*eg); } else *eg=-1;
    *alm=(ct>=CPU_T_HI)||(gt>=GPU_T_HI);
    int rc=(*ec>=0)?curve_rpm(*ec,CPU_T_LO,CPU_T_HI):0;
    int rg=(*eg>=0)?curve_rpm(*eg,GPU_T_LO,GPU_T_HI):0;
    int t=rc>rg?rc:rg; if(*alm)t=FAN_RPM_MAX; return (t/100)*100;
}

// ---------------- persisted state ----------------
// fanmode: 0=Auto 1=Manual 2=Curve. battery: -1=off(100%) else 50-100.
typedef struct { int mode,fanmode,fanrpm,kbd,battery; } State;
static State st = {0,0,4000,0,-1};
#define STATE_DIR "/etc/razerctld"
#define STATE_FILE STATE_DIR "/state.conf"
static void state_load(void){
    FILE*f=fopen(STATE_FILE,"r"); if(!f) return;
    char k[32]; int v;
    while(fscanf(f," %31[^=]=%d",k,&v)==2){
        if(!strcmp(k,"mode"))st.mode=v; else if(!strcmp(k,"fanmode"))st.fanmode=v;
        else if(!strcmp(k,"fanrpm"))st.fanrpm=v; else if(!strcmp(k,"kbd"))st.kbd=v;
        else if(!strcmp(k,"battery"))st.battery=v;
    }
    fclose(f);
}
static void state_save(void){
    mkdir(STATE_DIR,0755);
    char tmp[]=STATE_DIR "/state.conf.XXXXXX";
    int fd2=mkstemp(tmp); if(fd2<0) return;
    FILE*f=fdopen(fd2,"w"); if(!f){close(fd2);return;}
    fprintf(f,"mode=%d\nfanmode=%d\nfanrpm=%d\nkbd=%d\nbattery=%d\n",st.mode,st.fanmode,st.fanrpm,st.kbd,st.battery);
    fclose(f);
    rename(tmp,STATE_FILE);
}
static void apply_battery(int fd){ set_charge_limit(fd, st.battery<0?0x41:(st.battery|0x80)); }
static void apply_kbd(int fd){ if(st.kbd<=0) kbd_off(fd); else kbd_color(fd,KBD[st.kbd].r,KBD[st.kbd].g,KBD[st.kbd].b); }
static void apply_all(int fd){
    set_pmode(fd, st.mode, st.fanmode==1);
    apply_kbd(fd);
    apply_battery(fd);
    if(st.fanmode==0) set_fan_auto(fd);
    else if(st.fanmode==1) set_fan(fd,st.fanrpm);
    // fanmode==2 (Curve): the background loop drives it every tick, nothing to set now.
}

// ---------------- background loop: curve control + periodic drift reassert ----------------
// This is the ONLY thing that runs unattended. It's deliberately NOT the TUI's rich
// telemetry (tach/RAPL/battery-watts/GPU polling) -- that only happens on-demand, in
// direct response to a client request, so idle cost here stays a couple of hidraw
// round-trips every 30s (or every 2s while Curve is actively steering the fan).
#define REASSERT_S 300
static double ec_s=-1, eg_s=-1;
static int curve_applied=-1;
static double last_reassert=-1;
static void bg_tick(int fd){
    if(st.fanmode==2){
        int alarm; int target=curve_step(&ec_s,&eg_s,&alarm);
        if(curve_applied<0||alarm||abs(target-curve_applied)>=FAN_STEP){
            if(target==0){ if(set_fan_auto(fd)==0) curve_applied=0; }
            else if(set_fan(fd,target)==0) curve_applied=target;
        }
    }
    double now=mono();
    if(last_reassert<0 || now-last_reassert>=REASSERT_S){
        last_reassert=now;
        if(get_pmode(fd)!=st.mode) set_pmode(fd,st.mode,st.fanmode==1);
        apply_battery(fd);   // no readback exists for this one -- just re-send it
    }
}

// ---------------- client protocol ----------------
static void handle_client(int fd,int cfd){
    char buf[256]; int n=read(cfd,buf,sizeof buf-1);
    if(n<=0) return; buf[n]=0;
    char*nl=strpbrk(buf,"\r\n"); if(nl)*nl=0;
    char verb[32]={0}, rest[220]={0};
    sscanf(buf,"%31s %219[^\n]",verb,rest);
    char resp[256];
    if(!strcasecmp(verb,"PING")) snprintf(resp,sizeof resp,"OK PONG");
    else if(!strcasecmp(verb,"STATUS"))
        snprintf(resp,sizeof resp,"OK mode=%d fanmode=%d fanrpm=%d kbd=%d battery=%d",st.mode,st.fanmode,st.fanrpm,st.kbd,st.battery);
    else if(!strcasecmp(verb,"TACH")){ int f1=get_tach(fd,1),f2=get_tach(fd,2); snprintf(resp,sizeof resp,"OK %d %d",f1,f2); }
    else if(!strcasecmp(verb,"SETPOINT")){ int sp=get_fan_setpoint(fd); snprintf(resp,sizeof resp,"OK %d",sp); }
    else if(!strcasecmp(verb,"BATTERYRAW")){ int raw=get_charge_limit(fd); snprintf(resp,sizeof resp,"OK %d",raw); }
    else if(!strcasecmp(verb,"MODE")){
        int m=-1,force=0; sscanf(rest,"%d %d",&m,&force);
        if(m<0||m>2) snprintf(resp,sizeof resp,"ERR badarg");
        else { int r=set_pmode(fd,m,force||st.fanmode==1);
            if(r==0){ st.mode=m; state_save(); snprintf(resp,sizeof resp,"OK %d",m); }
            else snprintf(resp,sizeof resp,"ERR %d",r); }
    }
    else if(!strcasecmp(verb,"FAN")){
        char sub[16]={0}; int rpm=0; sscanf(rest,"%15s %d",sub,&rpm);
        if(!strcasecmp(sub,"AUTO")){ int r=set_fan_auto(fd);
            if(r==0){ st.fanmode=0; state_save(); curve_applied=-1; snprintf(resp,sizeof resp,"OK"); }
            else snprintf(resp,sizeof resp,"ERR"); }
        else if(!strcasecmp(sub,"MANUAL")){ if(rpm<2000)rpm=2000; if(rpm>4800)rpm=4800;
            int r=set_fan(fd,rpm);
            if(r==0){ st.fanmode=1; st.fanrpm=rpm; state_save(); curve_applied=-1; snprintf(resp,sizeof resp,"OK %d",rpm); }
            else snprintf(resp,sizeof resp,"ERR"); }
        else if(!strcasecmp(sub,"CURVE")){ st.fanmode=2; ec_s=eg_s=-1; curve_applied=-1; state_save(); snprintf(resp,sizeof resp,"OK"); }
        else snprintf(resp,sizeof resp,"ERR badsub");
    }
    else if(!strcasecmp(verb,"KBD")){
        int idx=atoi(rest);
        if(idx<0||idx>=NKBD) snprintf(resp,sizeof resp,"ERR badarg");
        else { int r = KBD[idx].r<0 ? kbd_off(fd) : kbd_color(fd,KBD[idx].r,KBD[idx].g,KBD[idx].b);
            if(r==0){ st.kbd=idx; state_save(); snprintf(resp,sizeof resp,"OK %d",idx); }
            else snprintf(resp,sizeof resp,"ERR"); }
    }
    else if(!strcasecmp(verb,"BATTERY")){
        if(!strcasecmp(rest,"OFF")){ int r=set_charge_limit(fd,0x41);
            if(r==0){ st.battery=-1; state_save(); snprintf(resp,sizeof resp,"OK OFF"); } else snprintf(resp,sizeof resp,"ERR"); }
        else { int p=atoi(rest);
            if(p<50||p>100) snprintf(resp,sizeof resp,"ERR badarg");
            else { int r=set_charge_limit(fd,p|0x80);
                if(r==0){ st.battery=p; state_save(); snprintf(resp,sizeof resp,"OK %d",p); } else snprintf(resp,sizeof resp,"ERR"); } }
    }
    else snprintf(resp,sizeof resp,"ERR unknown");
    size_t rn=strlen(resp); resp[rn++]='\n';
    ssize_t w=write(cfd,resp,rn); (void)w;
}

static volatile sig_atomic_t stop=0;
static void on_term(int s){ (void)s; stop=1; }

int main(void){
    if(geteuid()!=0){ fprintf(stderr,"razerctld needs root (run via systemd, not by hand)\n"); return 1; }
    signal(SIGTERM,on_term); signal(SIGINT,on_term); signal(SIGPIPE,SIG_IGN);
    state_load();

    char node[64]="?"; int fd=-1;
    for(int i=0;i<15 && fd<0 && !stop;i++){ fd=open_dev(node); if(fd<0) sleep(2); }
    if(fd<0){ fprintf(stderr,"razerctld: no responding 1532:02b7 hidraw after 30s, exiting (systemd will retry)\n"); return 2; }
    fprintf(stderr,"razerctld: EC found at %s, applying persisted state (mode=%d fanmode=%d kbd=%d battery=%d)\n",
        node,st.mode,st.fanmode,st.kbd,st.battery);
    apply_all(fd);

    unlink(RAZERCTLD_SOCK);
    int lfd=socket(AF_UNIX,SOCK_STREAM,0);
    if(lfd<0){ perror("socket"); return 1; }
    struct sockaddr_un addr; memset(&addr,0,sizeof addr); addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path,RAZERCTLD_SOCK,sizeof(addr.sun_path)-1);
    if(bind(lfd,(struct sockaddr*)&addr,sizeof addr)<0){ perror("bind"); return 1; }
    chmod(RAZERCTLD_SOCK,0666);   // single-user laptop; mirrors epp-write.conf/pcie-aspm-write.conf precedent
    listen(lfd,16);

    while(!stop){
        // Health probe doubles as the wait tick: while Curve is steering the fan we need a
        // 2s cadence anyway; otherwise back off to 30s so an idle daemon barely touches the EC.
        int wait_s = (st.fanmode==2) ? FAN_LOOP_S : 30;
        fd_set r; FD_ZERO(&r); FD_SET(lfd,&r);
        struct timeval tv={wait_s,0};
        int rv=select(lfd+1,&r,0,0,&tv);
        if(rv>0 && FD_ISSET(lfd,&r)){
            int cfd=accept(lfd,NULL,NULL);
            if(cfd>=0){ handle_client(fd,cfd); close(cfd); }
        }
        if(fd<0 || get_pmode(fd)<0){    // hidraw re-enumerates on suspend/resume -- reconnect + reapply
            if(fd>=0) close(fd);
            fd=open_dev(node);
            if(fd>=0){ fprintf(stderr,"razerctld: hidraw reconnected at %s, re-applying state\n",node); apply_all(fd); }
        }
        if(fd>=0) bg_tick(fd);
    }
    fprintf(stderr,"razerctld: shutting down (state stays applied; not reverting to AUTO)\n");
    unlink(RAZERCTLD_SOCK);
    return 0;
}
