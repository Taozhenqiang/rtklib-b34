/*------------------------------------------------------------------------------
* ppp.c : precise point positioning
*
*          Copyright (C) 2010-2020 by T.TAKASU, All rights reserved.
*
* options : -DIERS_MODEL  use IERS tide model
*           -DOUTSTAT_AMB output ambiguity parameters to solution status
*
* references :
*    [1] D.D.McCarthy, IERS Technical Note 21, IERS Conventions 1996, July 1996
*    [2] D.D.McCarthy and G.Petit, IERS Technical Note 32, IERS Conventions
*        2003, November 2003
*    [3] D.A.Vallado, Fundamentals of Astrodynamics and Applications 2nd ed,
*        Space Technology Library, 2004
*    [4] J.Kouba, A Guide to using International GNSS Service (IGS) products,
*        May 2009
*    [5] RTCM Paper, April 12, 2010, Proposed SSR Messages for SV Orbit Clock,
*        Code Biases, URA
*    [6] MacMillan et al., Atmospheric gradients and the VLBI terrestrial and
*        celestial reference frames, Geophys. Res. Let., 1997
*    [7] G.Petit and B.Luzum (eds), IERS Technical Note No. 36, IERS
*         Conventions (2010), 2010
*    [8] J.Kouba, A simplified yaw-attitude model for eclipsing GPS satellites,
*        GPS Solutions, 13:1-12, 2009
*    [9] F.Dilssner, GPS IIF-1 satellite antenna phase center and attitude
*        modeling, InsideGNSS, September, 2010
*    [10] F.Dilssner, The GLONASS-M satellite yaw-attitude model, Advances in
*        Space Research, 2010
*    [11] IGS MGEX (http://igs.org/mgex)
*
* version : $Revision:$ $Date:$
* history : 2010/07/20 1.0  new
*                           added api:
*                               tidedisp()
*           2010/12/11 1.1  enable exclusion of eclipsing satellite
*           2012/02/01 1.2  add gps-glonass h/w bias correction
*                           move windupcorr() to rtkcmn.c
*           2013/03/11 1.3  add otl and pole tides corrections
*                           involve iers model with -DIERS_MODEL
*                           change initial variances
*                           suppress acos domain error
*           2013/09/01 1.4  pole tide model by iers 2010
*                           add mode of ionosphere model off
*           2014/05/23 1.5  add output of trop gradient in solution status
*           2014/10/13 1.6  fix bug on P0(a[3]) computation in tide_oload()
*                           fix bug on m2 computation in tide_pole()
*           2015/03/19 1.7  fix bug on ionosphere correction for GLO and BDS
*           2015/05/10 1.8  add function to detect slip by MW-LC jump
*                           fix ppp solution problem with large clock variance
*           2015/06/08 1.9  add precise satellite yaw-models
*                           cope with day-boundary problem of satellite clock
*           2015/07/31 1.10 fix bug on nan-solution without glonass nav-data
*                           pppoutsolsat() -> pppoutstat()
*           2015/11/13 1.11 add L5-receiver-dcb estimation
*                           merge post-residual validation by rnx2rtkp_test
*                           support support option opt->pppopt=-GAP_RESION=nnnn
*           2016/01/22 1.12 delete support for yaw-model bug
*                           add support for ura of ephemeris
*           2018/10/10 1.13 support api change of satexclude()
*           2020/11/30 1.14 use sat2freq() to get carrier frequency
*                           use E1-E5b for Galileo iono-free LC
*-----------------------------------------------------------------------------*/
#include "rtklib.h"

#define SQR(x)      ((x)*(x))
#define SQRT(x)     ((x)<=0.0||(x)!=(x)?0.0:sqrt(x))
#define MAX(x,y)    ((x)>(y)?(x):(y))
#define MIN(x,y)    ((x)<(y)?(x):(y))
#define ROUND(x)    (int)floor((x)+0.5)

#define MAX_ITER    8               /* max number of iterations */
#define MAX_STD_FIX 0.15            /* max std-dev (3d) to fix solution */
#define MIN_NSAT_SOL 4              /* min satellite number for solution */
#define THRES_REJECT 4.0            /* reject threshold of posfit-res (sigma) */

#define THRES_MW_JUMP 10.0

#define VAR_POS     SQR(60.0)       /* init variance receiver position (m^2) */
#define VAR_VEL     SQR(10.0)       /* init variance of receiver vel ((m/s)^2) */
#define VAR_ACC     SQR(10.0)       /* init variance of receiver acc ((m/ss)^2) */
#define VAR_CLK     SQR(60.0)       /* init variance receiver clock (m^2) */
#define VAR_ZTD     SQR( 0.6)       /* init variance ztd (m^2) */
#define VAR_GRA     SQR(0.01)       /* init variance gradient (m^2) */
#define VAR_DCB     SQR(30.0)       /* init variance dcb (m^2) */
#define VAR_BIAS    SQR(60.0)       /* init variance phase-bias (m^2) */
#define VAR_IONO    SQR(60.0)       /* init variance iono-delay */
#define VAR_GLO_IFB SQR( 0.6)       /* variance of glonass ifb */

#define ERR_SAAS    0.3             /* saastamoinen model error std (m) */
#define ERR_BRDCI   0.5             /* broadcast iono model error factor */
#define ERR_CBIAS   0.3             /* code bias error std (m) */
#define REL_HUMI    0.7             /* relative humidity for saastamoinen model */
#define GAP_RESION  120             /* default gap to reset ionos parameters (ep) */

#define EFACT_GPS_L5 10.0           /* error factor of GPS/QZS L5 */

#define MUDOT_GPS   (0.00836*D2R)   /* average angular velocity GPS (rad/s) */
#define MUDOT_GLO   (0.00888*D2R)   /* average angular velocity GLO (rad/s) */
#define EPS0_GPS    (13.5*D2R)      /* max shadow crossing angle GPS (rad) */
#define EPS0_GLO    (14.2*D2R)      /* max shadow crossing angle GLO (rad) */
#define T_POSTSHADOW 1800.0         /* post-shadow recovery time (s) */
#define QZS_EC_BETA 20.0            /* max beta angle for qzss Ec (deg) */

/* number and index of states */
#define NF(opt)     ((opt)->ionoopt==IONOOPT_IFLC?1:(opt)->nf)
#define NP(opt)     ((opt)->dynamics?9:3)
#define NC(opt)     (NSYS)
#define NT(opt)     ((opt)->tropopt<TROPOPT_EST?0:((opt)->tropopt==TROPOPT_EST?1:3))
#define NI(opt)     ((opt)->ionoopt==IONOOPT_EST?MAXSAT:0)
#define ND(opt)     ((opt)->nf>=3?1:0)
#define NR(opt)     (NP(opt)+NC(opt)+NT(opt)+NI(opt)+ND(opt))
#define NB(opt)     (NF(opt)*MAXSAT)
#define NX(opt)     (NR(opt)+NB(opt))
#define IC(s,opt)   (NP(opt)+(s))
#define IT(opt)     (NP(opt)+NC(opt))
#define II(s,opt)   (NP(opt)+NC(opt)+NT(opt)+(s)-1)
#define ID(opt)     (NP(opt)+NC(opt)+NT(opt)+NI(opt))
#define IB(s,f,opt) (NR(opt)+MAXSAT*(f)+(s)-1)

/* standard deviation of state -----------------------------------------------*/
static double STD(rtk_t *rtk, int i)
{
    if (rtk->sol.stat==SOLQ_FIX) return SQRT(rtk->Pa[i+i*rtk->nx]);
    return SQRT(rtk->P[i+i*rtk->nx]);
}
/* write solution status for PPP ---------------------------------------------*/
extern int pppoutstat(rtk_t *rtk, char *buff)
{
    ssat_t *ssat;
    double tow,pos[3],vel[3],acc[3],*x;
    int i,j,week;
    char id[32],*p=buff;

    if (!rtk->sol.stat) return 0;

    trace(3,"pppoutstat:\n");

    tow=time2gpst(rtk->sol.time,&week);

    x=rtk->sol.stat==SOLQ_FIX?rtk->xa:rtk->x;

    /* receiver position */
    p+=sprintf(p,"$POS,%d,%.3f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",week,tow,
               rtk->sol.stat,x[0],x[1],x[2],STD(rtk,0),STD(rtk,1),STD(rtk,2));

    /* receiver velocity and acceleration */
    if (rtk->opt.dynamics) {
        ecef2pos(rtk->sol.rr,pos);
        ecef2enu(pos,rtk->x+3,vel);
        ecef2enu(pos,rtk->x+6,acc);
        p+=sprintf(p,"$VELACC,%d,%.3f,%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.4f,%.4f,"
                   "%.4f,%.5f,%.5f,%.5f\n",week,tow,rtk->sol.stat,vel[0],vel[1],
                   vel[2],acc[0],acc[1],acc[2],0.0,0.0,0.0,0.0,0.0,0.0);
    }
    /* receiver clocks */
    i=IC(0,&rtk->opt);
    p+=sprintf(p,"$CLK,%d,%.3f,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
               week,tow,rtk->sol.stat,1,x[i]*1E9/CLIGHT,x[i+1]*1E9/CLIGHT,
               x[i+2]*1E9/CLIGHT,x[i+3]*1E9/CLIGHT,STD(rtk,i)*1E9/CLIGHT,
               STD(rtk,i+1)*1E9/CLIGHT,STD(rtk,i+2)*1E9/CLIGHT,
               STD(rtk,i+2)*1E9/CLIGHT);

    /* tropospheric parameters */
    if (rtk->opt.tropopt==TROPOPT_EST||rtk->opt.tropopt==TROPOPT_ESTG) {
        i=IT(&rtk->opt);
        p+=sprintf(p,"$TROP,%d,%.3f,%d,%d,%.4f,%.4f\n",week,tow,rtk->sol.stat,
                   1,x[i],STD(rtk,i));
    }
    if (rtk->opt.tropopt==TROPOPT_ESTG) {
        i=IT(&rtk->opt);
        p+=sprintf(p,"$TRPG,%d,%.3f,%d,%d,%.5f,%.5f,%.5f,%.5f\n",week,tow,
                   rtk->sol.stat,1,x[i+1],x[i+2],STD(rtk,i+1),STD(rtk,i+2));
    }
    /* ionosphere parameters */
    if (rtk->opt.ionoopt==IONOOPT_EST) {
        for (i=0;i<MAXSAT;i++) {
            ssat=rtk->ssat+i;
            if (!ssat->vs) continue;
            j=II(i+1,&rtk->opt);
            if (rtk->x[j]==0.0) continue;
            satno2id(i+1,id);
            p+=sprintf(p,"$ION,%d,%.3f,%d,%s,%.1f,%.1f,%.4f,%.4f\n",week,tow,
                       rtk->sol.stat,id,rtk->ssat[i].azel[0]*R2D,
                       rtk->ssat[i].azel[1]*R2D,x[j],STD(rtk,j));
        }
    }
#ifdef OUTSTAT_AMB
    /* ambiguity parameters */
    int k;
    for (i=0;i<MAXSAT;i++) for (j=0;j<NF(&rtk->opt);j++) {
        k=IB(i+1,j,&rtk->opt);
        if (rtk->x[k]==0.0) continue;
        satno2id(i+1,id);
        p+=sprintf(p,"$AMB,%d,%.3f,%d,%s,%d,%.4f,%.4f\n",week,tow,
                   rtk->sol.stat,id,j+1,x[k],STD(rtk,k));
    }
#endif
    return (int)(p-buff);
}
/* exclude meas of eclipsing satellite (block IIA) ---------------------------*/
static void testeclipse(const obsd_t *obs, int n, const nav_t *nav, double *rs)
{
    double rsun[3],esun[3],r,ang,erpv[5]={0},cosa;
    int i,j;
    const char *type;

    trace(3,"testeclipse:\n");

    /* unit vector of sun direction (ecef) */
    sunmoonpos(gpst2utc(obs[0].time),erpv,rsun,NULL,NULL);
    normv3(rsun,esun);

    for (i=0;i<n;i++) {
        type=nav->spcvs[obs[i].sat-1].type;

        if ((r=norm(rs+i*6,3))<=0.0) continue;

        /* only block IIA */
        if (*type&&!strstr(type,"BLOCK IIA")) continue;

        /* sun-earth-satellite angle */
        cosa=dot3(rs+i*6,esun)/r;
        cosa=cosa<-1.0?-1.0:(cosa>1.0?1.0:cosa);
        ang=acos(cosa);

        /* test eclipse */
        if (ang<PI/2.0||r*sin(ang)>RE_WGS84) continue;

        trace(3,"eclipsing sat excluded %s sat=%2d\n",time_str(obs[0].time,0),
              obs[i].sat);

        for (j=0;j<3;j++) rs[j+i*6]=0.0;
    }
}
/* nominal yaw-angle ---------------------------------------------------------*/
static double yaw_nominal(double beta, double mu)
{
    if (fabs(beta)<1E-12&&fabs(mu)<1E-12) return PI;
    return atan2(-tan(beta),sin(mu))+PI;
}
/* yaw-angle of satellite ----------------------------------------------------*/
extern int yaw_angle(int sat, const char *type, int opt, double beta, double mu,
                     double *yaw)
{
    *yaw=yaw_nominal(beta,mu);
    return 1;
}
/* satellite attitude model --------------------------------------------------*/
static int sat_yaw(gtime_t time, int sat, const char *type, int opt,
                   const double *rs, double *exs, double *eys)
{
    double rsun[3],ri[6],es[3],esun[3],n[3],p[3],en[3],ep[3],ex[3],E,beta,mu;
    double yaw,cosy,siny,erpv[5]={0};
    int i;

    sunmoonpos(gpst2utc(time),erpv,rsun,NULL,NULL);

    /* beta and orbit angle */
    matcpy(ri,rs,6,1);
    ri[3]-=OMGE*ri[1];
    ri[4]+=OMGE*ri[0];
    cross3(ri,ri+3,n);
    cross3(rsun,n,p);
    if (!normv3(rs,es)||!normv3(rsun,esun)||!normv3(n,en)||
        !normv3(p,ep)) return 0;
    beta=PI/2.0-acos(dot3(esun,en));
    E=acos(dot3(es,ep));
    mu=PI/2.0+(dot3(es,esun)<=0?-E:E);
    if      (mu<-PI/2.0) mu+=2.0*PI;
    else if (mu>=PI/2.0) mu-=2.0*PI;

    /* yaw-angle of satellite */
    if (!yaw_angle(sat,type,opt,beta,mu,&yaw)) return 0;

    /* satellite fixed x,y-vector */
    cross3(en,es,ex);
    cosy=cos(yaw);
    siny=sin(yaw);
    for (i=0;i<3;i++) {
        exs[i]=-siny*en[i]+cosy*ex[i];
        eys[i]=-cosy*en[i]-siny*ex[i];
    }
    return 1;
}
/* phase windup model --------------------------------------------------------*/
static int model_phw(gtime_t time, int sat, const char *type, int opt,
                     const double *rs, const double *rr, double *phw)
{
    double exs[3],eys[3],ek[3],exr[3],eyr[3],eks[3],ekr[3],E[9];
    double dr[3],ds[3],drs[3],r[3],pos[3],cosp,ph;
    int i;

    if (opt<=0) return 1; /* no phase windup */

    /* satellite yaw attitude model */
    if (!sat_yaw(time,sat,type,opt,rs,exs,eys)) return 0;

    /* unit vector satellite to receiver */
    for (i=0;i<3;i++) r[i]=rr[i]-rs[i];
    if (!normv3(r,ek)) return 0;

    /* unit vectors of receiver antenna */
    ecef2pos(rr,pos);
    xyz2enu(pos,E);
    exr[0]= E[1]; exr[1]= E[4]; exr[2]= E[7]; /* x = north */
    eyr[0]=-E[0]; eyr[1]=-E[3]; eyr[2]=-E[6]; /* y = west  */

    /* phase windup effect */
    cross3(ek,eys,eks);
    cross3(ek,eyr,ekr);
    for (i=0;i<3;i++) {
        ds[i]=exs[i]-ek[i]*dot3(ek,exs)-eks[i];
        dr[i]=exr[i]-ek[i]*dot3(ek,exr)+ekr[i];
    }
    cosp=dot3(ds,dr)/norm(ds,3)/norm(dr,3);
    if      (cosp<-1.0) cosp=-1.0;
    else if (cosp> 1.0) cosp= 1.0;
    ph=acos(cosp)/2.0/PI;
    cross3(ds,dr,drs);
    if (dot3(ek,drs)<0.0) ph=-ph;

    *phw=ph+floor(*phw-ph+0.5); /* in cycle */
    return 1;
}
/* measurement error variance ------------------------------------------------*/
static double varerr(int sat, int sys, double el, double snr_rover,
                     int f, const prcopt_t *opt, const obsd_t *obs)
{
    double a,b,e;
    double snr_max=opt->err[5];
    double fact=1.0;
    double sinel=sin(el),var;
    int frq,code;

    frq=f/2;code=f%2; /* 0=phase, 1=code */
    /* increase variance for pseudoranges */
    if (code) fact=opt->eratio[frq];
    if (fact<=0.0) fact=opt->eratio[0];
    /* adjust variances for constellation */
    switch (sys) {
        case SYS_GPS: fact*=EFACT_GPS;break;
        case SYS_GLO: fact*=EFACT_GLO;break;
        case SYS_GAL: fact*=EFACT_GAL;break;
        case SYS_SBS: fact*=EFACT_SBS;break;
        case SYS_QZS: fact*=EFACT_QZS;break;
        case SYS_CMP: fact*=EFACT_CMP;break;
        case SYS_IRN: fact*=EFACT_IRN;break;
        default:      fact*=EFACT_GPS;break;
    }
    if (sys==SYS_GPS||sys==SYS_QZS) {
        if (frq==2) fact*=EFACT_GPS_L5; /* GPS/QZS L5 error factor */
    }
    /* adjust variance for config parameters */
    a=fact*opt->err[1];  /* base term */
    b=fact*opt->err[2];  /* el term */
    /* calculate variance */
    var=(a*a+b*b/sinel/sinel);
    if (opt->err[6]>0) {  /* add SNR term */
        e=fact*opt->err[6];
        var+=e*e*(pow(10,0.1*MAX(snr_max-snr_rover,0)));
    }
    if (opt->err[7]>0.0) {   /* add rcvr stdevs term */
        if (code) var+=SQR(opt->err[7]*0.01*(1<<(obs->Pstd[frq]+5))); /* 0.01*2^(n+5) */
        else var+=SQR(opt->err[7]*obs->Lstd[frq]*0.004*0.2); /* 0.004 cycles -> m) */
    }
    /* FIXME: the scaling factor is not 3 for other signals/constellations than GPS L1/L2 */
    var*=(opt->ionoopt==IONOOPT_IFLC)?SQR(3.0):1.0;
    return var;
}
/* initialize state and covariance -------------------------------------------*/
static inline void initx(rtk_t *rtk, double xi, double var, int i)
{
    int j;
    rtk->x[i]=xi;
    for (j=0;j<rtk->nx;j++) rtk->P[i+j*rtk->nx]=0.0;
    for (j=0;j<rtk->nx;j++) rtk->P[j+i*rtk->nx]=0.0;
    rtk->P[i+i*rtk->nx]=var;
}
/* geometry-free phase measurement -------------------------------------------*/
static double gfmeas(const obsd_t *obs, const nav_t *nav, int i, int j)
{
    double freq1,freq2;

    freq1=sat2freq(obs->sat,obs->code[i],nav);
    freq2=sat2freq(obs->sat,obs->code[j],nav);
    if (freq1==0.0||freq2==0.0||obs->L[i]==0.0||obs->L[j]==0.0) return 0.0;
    return (obs->L[i]/freq1-obs->L[j]/freq2)*CLIGHT;
}
/* Melbourne-Wubbena linear combination --------------------------------------*/
static double mwmeas(const obsd_t *obs, const nav_t *nav, int i, int j)
{
    double freq1,freq2;

    freq1=sat2freq(obs->sat,obs->code[i],nav);
    freq2=sat2freq(obs->sat,obs->code[j],nav);

    if (freq1==0.0||freq2==0.0||obs->L[i]==0.0||obs->L[j]==0.0||
        obs->P[i]==0.0||obs->P[j]==0.0) return 0.0;
    trace(3,"mwmeas: %12.1f %12.1f %15.3f %15.3f %15.3f %15.3f %d %d\n",freq1,freq2,obs->L[i],obs->L[j],obs->P[i],obs->P[j],obs->code[i],obs->code[j]);
    return (obs->L[i]-obs->L[j])*CLIGHT/(freq1-freq2)-
           (freq1*obs->P[i]+freq2*obs->P[j])/(freq1+freq2);
}
/* antenna corrected measurements --------------------------------------------*/
static void corr_meas(const obsd_t *obs, const nav_t *nav, const double *azel,
                      const prcopt_t *opt, const double *dantr,
                      const double *dants, double phw, double *L, double *P,
                      double *Lc, double *Pc)
{
    double freq[NFREQ]={0},C1,C2;
    int i,ix=0,frq,frq2,bias_ix,sys=satsys(obs->sat,NULL),fr;

    for (i=0;i<opt->nf;i++) {
        fr=sys2freid(sys,i,opt);
        L[i]=P[i]=0.0;
        /* skip if low SNR or missing observations */
        freq[i]=sat2freq(obs->sat,obs->code[fr],nav);
        if (freq[i]==0.0||obs->L[fr]==0.0||obs->P[fr]==0.0) continue;
        if (testsnr(0,fr,azel[1],obs->SNR[fr]*SNR_UNIT,&opt->snrmask)) continue;

        /* antenna phase center and phase windup correction */
        L[i]=obs->L[fr]*CLIGHT/freq[i]-dants[fr]-dantr[fr]-phw*CLIGHT/freq[i];
        P[i]=obs->P[fr]               -dants[fr]-dantr[fr];

        /* P1-C1,P2-C2 DCB correction */
        if (sys==SYS_GPS||sys==SYS_GLO) {
            if (obs->code[fr]==CODE_L1C) P[i]+=nav->cbias[obs->sat-1][0]; /* C1->P1 */
            if (obs->code[fr]==CODE_L2C) P[i]+=nav->cbias[obs->sat-1][1]; /* C2->P2 */
        }

        if (opt->sateph==EPHOPT_SSRAPC||opt->sateph==EPHOPT_SSRCOM) {
            /* select SSR code correction based on code */
            if (sys==SYS_GPS)
                ix=(i==0?CODE_L1W-1:CODE_L2W-1);
            else if (sys==SYS_GLO)
                ix=(i==0?CODE_L1P-1:CODE_L2P-1);
            else if (sys==SYS_GAL)
                ix=(i==0?CODE_L1X-1:CODE_L7X-1);
            /* apply SSR correction */
            P[i]+=(nav->ssr[obs->sat-1].cbias[obs->code[i]-1]-nav->ssr[obs->sat-1].cbias[ix]);
        }
        else {   
            /* apply code bias corrections from file */
            bias_ix=code2bias_ix(sys,obs->code[fr]); /* look up bias index in table */
            P[i]-=nav->obias[obs->sat-1][bias_ix]; /*DCB/OSB*/
        }
    }
    /* choose freqs for iono-free LC */
    *Lc=*Pc=0.0;
    frq2=L[1]==0?2:1;  /* if L[1]==0, try L[2] */
    if (freq[0]==0.0||freq[frq2]==0.0) return;
    C1= SQR(freq[0])/(SQR(freq[0])-SQR(freq[frq2]));
    C2=-SQR(freq[frq2])/(SQR(freq[0])-SQR(freq[frq2]));

    if (L[0]!=0.0&&L[frq2]!=0.0) *Lc=C1*L[0]+C2*L[frq2];
    if (P[0]!=0.0&&P[frq2]!=0.0) *Pc=C1*P[0]+C2*P[frq2];
}
/* detect cycle slip by LLI --------------------------------------------------*/
static void detslp_ll(rtk_t *rtk, const obsd_t *obs, int n)
{
    prcopt_t *opt=&rtk->opt;
    int i,j,nf=rtk->opt.nf,sys,fr;

    trace(3,"detslp_ll: n=%d\n",n);

    for (i=0;i<n&&i<MAXOBS;i++) {
        sys=satsys(obs[i].sat,NULL);
        for (j=0;j<rtk->opt.nf;j++) {
            fr=sys2freid(sys,j,opt);

            if (obs[i].L[fr]==0.0||!(obs[i].LLI[fr]&3)) continue;

            trace(3,"detslp_ll: slip detected sat=%2d f=%d\n",obs[i].sat,fr+1);

            rtk->ssat[obs[i].sat-1].slip[fr]=1;
        }  
    }
}
/* detect cycle slip by geometry free phase jump -----------------------------*/
static void detslp_gf(rtk_t *rtk, const obsd_t *obs, int n, const nav_t *nav)
{
    prcopt_t *opt=&rtk->opt;
    double g0,g1,el;
    int i,j,sys,fr,fr2[2];

    trace(5,"detslp_gf: n=%d\n",n);

    for (i=0;i<n&&i<MAXOBS;i++) {
        el=rtk->ssat[obs[i].sat-1].azel[1]*R2D;
        sys=satsys(obs[i].sat,NULL);
        fr2[0]=sys2freid(sys,0,opt);

        for (j=1;j<rtk->opt.nf;j++) {
            fr2[1]=sys2freid(sys,j,opt);
            if ((g1=gfmeas(obs+i,nav,fr2[0],fr2[1]))==0.0) continue;

            g0=rtk->ssat[obs[i].sat-1].gf[fr2[1]-1];
            rtk->ssat[obs[i].sat-1].gf[fr2[1]-1]=g1;

            trace(5,"%s GF: slip detected gf_new=%13.3f, gf_old=%13.3f, diff_abs=%13.3f, thres=%13.3f, el=%4.1f\n",
                  rtk->ssat[obs[i].sat-1].id,g1,g0,fabs(g1-g0),rtk->opt.thresslip,el);

            if (g0!=0.0&&fabs(g1-g0)>rtk->opt.thresslip) {
                trace(4,"%s GF: slip detected gf_new=%13.3f, gf_old=%13.3f, diff_abs=%13.3f, thres=%13.3f, el=%4.1f\n",
                      rtk->ssat[obs[i].sat-1].id,g1,g0,fabs(g1-g0),rtk->opt.thresslip,el);

                rtk->ssat[obs[i].sat-1].slip[fr2[0]]|=1;
                rtk->ssat[obs[i].sat-1].slip[fr2[1]]|=1;
                }
        }
    }
}
/* detect slip by Melbourne-Wubbena linear combination jump ------------------*/
static void detslp_mw(rtk_t *rtk, const obsd_t *obs, int n, const nav_t *nav)
{
    prcopt_t *opt=&rtk->opt;
    double w0,w1,el;
    int i,j,sys,fr2[2];

    trace(5,"detslp_mw: n=%d\n",n);
    
    for (i=0;i<n&&i<MAXOBS;i++) {
        el=rtk->ssat[obs[i].sat-1].azel[1]*R2D;
        sys=satsys(obs[i].sat,NULL);
        fr2[0]=sys2freid(sys,0,opt);

        for (j=1;j<rtk->opt.nf;j++) {
            fr2[1]=sys2freid(sys,j,opt);
            if ((w1=mwmeas(obs+i,nav,fr2[0],fr2[1]))==0.0) continue;

            w0=rtk->ssat[obs[i].sat-1].mw[fr2[1]-1];
            rtk->ssat[obs[i].sat-1].mw[fr2[1]-1]=w1;

            trace(5,"%s MW: slip detected mw_new=%13.3f, mw_old=%13.3f, diff_abs=%13.3f, thres=%13.3f, el=%4.1f\n",
                  rtk->ssat[obs[i].sat-1].id,w1,w0,fabs(w1-w0),THRES_MW_JUMP,el);

            if (w0!=0.0&&fabs(w1-w0)>THRES_MW_JUMP) {
                trace(4,"%s MW: slip detected mw_new=%13.3f, mw_old=%13.3f, diff_abs=%13.3f, thres=%13.3f, el=%4.1f\n",
                      rtk->ssat[obs[i].sat-1].id,w1,w0,fabs(w1-w0),THRES_MW_JUMP,el);

                rtk->ssat[obs[i].sat-1].slip[fr2[0]]|=1;
                rtk->ssat[obs[i].sat-1].slip[fr2[1]]|=1;
                }
        } 
    }
}
/* temporal update of position -----------------------------------------------*/
static void udpos_ppp(rtk_t *rtk)
{
    double *F,*P,*FP,*x,*xp,pos[3],Q[9]={0},Qv[9],var=0.0;
    int i,j,*ix,nx;

    trace(3,"udpos_ppp:\n");

    /* fixed mode */
    if (rtk->opt.mode==PMODE_PPP_FIXED) {
        for (i=0;i<3;i++) initx(rtk,rtk->opt.ru[i],1E-8,i);
        return;
    }
    /* initialize position for first epoch */
    if (norm(rtk->x,3)<=0.0) {
        for (i=0;i<3;i++) initx(rtk,rtk->sol.rr[i],VAR_POS,i);
        if (rtk->opt.dynamics) {
            for (i=3;i<6;i++) initx(rtk,rtk->sol.rr[i],VAR_VEL,i);
            for (i=6;i<9;i++) initx(rtk,1E-6,VAR_ACC,i);
        }
    }
    /* static ppp mode */
    if (rtk->opt.mode==PMODE_PPP_STATIC) {
        for (i=0;i<3;i++) {
            rtk->P[i*(1+rtk->nx)]+=SQR(rtk->opt.prn[5])*fabs(rtk->tt);
        }
        return;
    }
    /* kinematic mode without dynamics */
    if (!rtk->opt.dynamics) {
        for (i=0;i<3;i++) {
            initx(rtk,rtk->sol.rr[i],VAR_POS,i);
        }
        return;
    }
    /* check variance of estimated position */
    for (i=0;i<3;i++) var+=rtk->P[i+i*rtk->nx];
    var/=3.0;

    if (var>VAR_POS) {
        /* reset position with large variance */
        for (i=0;i<3;i++) initx(rtk,rtk->sol.rr[i],VAR_POS,i);
        for (i=3;i<6;i++) initx(rtk,rtk->sol.rr[i],VAR_VEL,i);
        for (i=6;i<9;i++) initx(rtk,1E-6,VAR_ACC,i);
        trace(2,"reset rtk position due to large variance: var=%.3f\n",var);
        return;
    }
    /* generate valid state index */
    ix=imat(rtk->nx,1);
    for (i=nx=0;i<rtk->nx;i++) {
        if  (i<9||(rtk->x[i]!=0.0&&rtk->P[i+i*rtk->nx]>0.0)) ix[nx++]=i;
    }
    /* state transition of position/velocity/acceleration */
    F=eye(nx); P=mat(nx,nx); FP=mat(nx,nx); x=mat(nx,1); xp=mat(nx,1);

    for (i=0;i<6;i++) {
        F[i+(i+3)*nx]=rtk->tt;
    }
    /* include accel terms if filter is converged */
    if (var<rtk->opt.thresar[1]) {
        for (i=0;i<3;i++) {
            F[i+(i+6)*nx]=SQR(rtk->tt)/2.0;
        }
    }
    else trace(3,"pos var too high for accel term: %.4f,%.4f\n", var,rtk->opt.thresar[1]);
    for (i=0;i<nx;i++) {
        x[i]=rtk->x[ix[i]];
        for (j=0;j<nx;j++) {
            P[i+j*nx]=rtk->P[ix[i]+ix[j]*rtk->nx];
        }
    }
    /* x=F*x, P=F*P*F+Q */
    matmul("NN",nx,1,nx,F,x,xp);
    matmul("NN",nx,nx,nx,F,P,FP);
    matmul("NT",nx,nx,nx,FP,F,P);

    for (i=0;i<nx;i++) {
        rtk->x[ix[i]]=xp[i];
        for (j=0;j<nx;j++) {
            rtk->P[ix[i]+ix[j]*rtk->nx]=P[i+j*nx];
        }
    }
    /* process noise added to only acceleration */
    Q[0]=Q[4]=SQR(rtk->opt.prn[3])*fabs(rtk->tt);
    Q[8]=SQR(rtk->opt.prn[4])*fabs(rtk->tt);
    ecef2pos(rtk->x,pos);
    covecef(pos,Q,Qv);
    for (i=0;i<3;i++) for (j=0;j<3;j++) {
        rtk->P[i+6+(j+6)*rtk->nx]+=Qv[i+j*3];
    }
    free(ix); free(F); free(P); free(FP); free(x); free(xp);
}
/* temporal update of clock --------------------------------------------------*/
static void udclk_ppp(rtk_t *rtk)
{
    prcopt_t *opt=&rtk->opt;
    double dtr;
    int i,ic,sys=rtk->opt.navsys;
    trace(3,"udclk_ppp:\n");

    /*Single system clock error initialization (no GPS)*/
    if (sys==SYS_CMP) {
		dtr=rtk->sol.dtr[0]+rtk->sol.dtr[3];
		if (fabs(dtr)<1.0e-16) dtr=1.0e-16;
		ic=IC(3,opt);
		initx(rtk,CLIGHT*dtr,VAR_CLK,ic);

		return;
	}
	else if (sys==SYS_GAL) {
		dtr=rtk->sol.dtr[0]+rtk->sol.dtr[2];
		if (fabs(dtr)<1.0e-16) dtr=1.0e-16;
		ic=IC(2,opt);
		initx(rtk,CLIGHT*dtr,VAR_CLK,ic);

		return;
	}

	/* initialize GPS clock (white noise) */
	dtr=rtk->sol.dtr[0];
	if (fabs(dtr)<1.0e-16) dtr=1.0e-16;
	ic=IC(0,opt);
	initx(rtk,CLIGHT*dtr,VAR_CLK,ic);

    /* Multi system clock error initialization */
    for (i=1;i<NSYS;i++) {
        if (!(sys&SYS_GLO)&&i==1) continue;
        if (!(sys&SYS_GAL)&&i==2) continue;
        if (!(sys&SYS_CMP)&&i==3) continue;
        if (!(sys&SYS_IRN)&&i==4) continue;
        if (!(sys&SYS_QZS)&&i==5) continue;

        dtr=rtk->sol.dtr[i];
        ic=IC(i,opt);

        if (opt->sysisb==GNSISB_CT) {
            // constant
            if (rtk->x[ic]==0.0) {
                if (fabs(dtr)<1.0e-16) dtr=1.0e-16;
                initx(rtk,CLIGHT*dtr,VAR_CLK,ic);
            }
        }
        else if (opt->sysisb==GNSISB_RW) {
            // random walk process
            if (rtk->x[ic]==0.0) {
                if (fabs(dtr)<1.0e-16) dtr=1.0e-16;
                initx(rtk,CLIGHT*dtr,VAR_CLK,ic);
            }  
            else {
                rtk->P[ic+ic*rtk->nx]+=SQR(0.001)*fabs(rtk->tt);
            }
        }
        else if (opt->sysisb==GNSISB_WN) {
            //white noise process
            if (fabs(dtr)<1.0e-16) dtr=1.0e-16;
            initx(rtk,CLIGHT*dtr,VAR_CLK,ic);
        }
    }
}
/* temporal update of tropospheric parameters --------------------------------*/
static void udtrop_ppp(rtk_t *rtk)
{
    double pos[3],azel[]={0.0,PI/2.0},ztd,var;
    int i=IT(&rtk->opt),j;

    trace(3,"udtrop_ppp:\n");

    if (rtk->x[i]==0.0) {
        ecef2pos(rtk->sol.rr,pos);
        ztd=sbstropcorr(rtk->sol.time,pos,azel,&var);
        initx(rtk,ztd,var,i);

        if (rtk->opt.tropopt>=TROPOPT_ESTG) {
            for (j=i+1;j<i+3;j++) initx(rtk,1E-6,VAR_GRA,j);
        }
    }
    else {
        rtk->P[i+i*rtk->nx]+=SQR(rtk->opt.prn[2])*fabs(rtk->tt);

        if (rtk->opt.tropopt>=TROPOPT_ESTG) {
            for (j=i+1;j<i+3;j++) {
                rtk->P[j+j*rtk->nx]+=SQR(rtk->opt.prn[2]*0.1)*fabs(rtk->tt);
            }
        }
    }
}
/* temporal update of ionospheric parameters ---------------------------------*/
static void udiono_ppp(rtk_t *rtk, const obsd_t *obs, int n, const nav_t *nav)
{
    prcopt_t *opt=&rtk->opt;
    double freq1,freq2,ion,sinel,pos[3],*azel;
    char *p;
    int i,j,f2,gap_resion=GAP_RESION,sat,sys,fr,fr2[2],el;

    trace(3,"udiono_ppp:\n");

    if ((p=strstr(rtk->opt.pppopt,"-GAP_RESION="))) {
        sscanf(p,"-GAP_RESION=%d",&gap_resion);
    }
    /* reset ionosphere delay estimate if outage too long */
    for (i=0;i<MAXSAT;i++) {
        sys=satsys(i+1,NULL);
        fr=sys2freid(sys,0,opt);
        j=II(i+1,&rtk->opt);
        if (rtk->x[j]!=0.0&&(int)rtk->ssat[i].outc[fr]>gap_resion) {
            rtk->x[j]=0.0;
        }
    }
    for (i=0;i<n;i++) {
        sat=obs[i].sat;
        j=II(sat,&rtk->opt);
        el=rtk->ssat[sat-1].azel[1]*R2D;
        if (rtk->x[j]==0.0) {
            /* initialize ionosphere delay estimates if zero */
            /* f2=seliflc(rtk->opt.nf,satsys(sat,NULL)); */
            sys=satsys(sat,NULL);
            fr2[0]=sys2freid(sys,0,opt);
            fr2[1]=sys2freid(sys,1,opt);
            freq1=sat2freq(sat,obs[i].code[fr2[0]],nav);
            freq2=sat2freq(sat,obs[i].code[fr2[1]],nav);
            if (obs[i].P[fr2[0]]==0.0||obs[i].P[fr2[1]]==0.0||freq1==0.0||freq2==0.0) {
                continue;
            }
            /* use pseudorange difference adjusted by freq for initial estimate, based on GPS L1 frequency */
            ion=(obs[i].P[fr2[0]]-obs[i].P[fr2[1]])/(SQR(FREQL1/freq1)-SQR(FREQL1/freq2));
            ecef2pos(rtk->sol.rr,pos);
            azel=rtk->ssat[sat-1].azel;
            /* adjust delay estimate by path length */
            /* ion/=ionmapf(pos,azel); */
            initx(rtk,ion,VAR_IONO,j);
            trace(9,"ion init: sat=%d ion=%.4f\n",sat,ion);
        }
        else {
            sinel=sin(MAX(rtk->ssat[sat-1].azel[1],5.0*D2R));
            /* update variance of delay state */
            if (el>=30) {
                rtk->P[j+j*rtk->nx]+=SQR(rtk->opt.prn[1])*fabs(rtk->tt);
            }
            else {
                rtk->P[j+j*rtk->nx]+=SQR(rtk->opt.prn[1]/sinel)*fabs(rtk->tt); 
            }   
        }
    }
}
/* temporal update of L5-receiver-dcb parameters -----------------------------*/
static void uddcb_ppp(rtk_t *rtk)
{
    int i=ID(&rtk->opt);

    trace(3,"uddcb_ppp:\n");

    if (rtk->x[i]==0.0) {
        initx(rtk,1E-6,VAR_DCB,i);
    }
}
/* temporal update of phase biases -------------------------------------------*/
static void udbias_ppp(rtk_t *rtk, const obsd_t *obs, int n, const nav_t *nav)
{
    prcopt_t *opt=&rtk->opt;
    double L[NFREQ],P[NFREQ],Lc,Pc,bias[MAXOBS],offset=0.0,pos[3]={0};
    double freq,freq1,freq2,ion,dantr[NFREQ]={0},dants[NFREQ]={0};
    int i,j,k,f,sat,slip[MAXOBS]={0},clk_jump=0,sys,fr,fr2[2],nf=rtk->opt.nf;

    trace(3,"udbias  : n=%d\n",n);

    /* handle day-boundary clock jump */
    if (rtk->opt.posopt[5]) {
        clk_jump=ROUND(time2gpst(obs[0].time,NULL)*10)%864000==0;
    }
    for (i=0;i<MAXSAT;i++) {
        sys=satsys(i+1,NULL);
        for (j=0;j<rtk->opt.nf;j++) {
            fr=sys2freid(sys,j,opt);
            rtk->ssat[i].slip[fr]=0;
        }
    }
    /* detect cycle slip by LLI */
    /* detslp_ll(rtk,obs,n); */

    /* detect cycle slip by geometry-free phase jump */
    detslp_gf(rtk,obs,n,nav);

    /* detect slip by Melbourne-Wubbena linear combination jump */
    detslp_mw(rtk,obs,n,nav);

    ecef2pos(rtk->sol.rr,pos);

    for (f=0;f<NF(&rtk->opt);f++) {

        /* reset phase-bias if expire obs outage counter */
        for (i=0;i<MAXSAT;i++) {
            sys=satsys(i+1,NULL);
            fr=sys2freid(sys,f,opt);

            if (++rtk->ssat[i].outc[fr]>(uint32_t)rtk->opt.maxout||
                rtk->opt.modear==ARMODE_INST||clk_jump) {
                initx(rtk,0.0,0.0,IB(i+1,f,&rtk->opt));
            }
        }
        for (i=k=0;i<n&&i<MAXOBS;i++) {
            sat=obs[i].sat;
            sys=satsys(sat,NULL);
            fr=sys2freid(sys,f,opt);
            fr2[0]=sys2freid(sys,0,opt);
            fr2[1]=sys2freid(sys,1,opt);

            j=IB(sat,f,&rtk->opt);
            corr_meas(obs+i,nav,rtk->ssat[sat-1].azel,&rtk->opt,dantr,dants,
                      0.0,L,P,&Lc,&Pc);

            bias[i]=0.0;

            if (rtk->opt.ionoopt==IONOOPT_IFLC) {      
                bias[i]=Lc-Pc;
                slip[i]=rtk->ssat[sat-1].slip[fr2[0]]||rtk->ssat[sat-1].slip[fr2[1]];
            }
            else if (L[f]!=0.0&&P[f]!=0.0) {
                if (fabs(P[f]-P[0])>1000) {
                    trace(6," The code/phase observation is missing or invalid. sat=%s, L%d\n",rtk->ssat[sat-1].id,fr+1);
                    continue;
                }
                freq=sat2freq(sat,obs[i].code[fr],nav);
                freq1=sat2freq(sat,obs[i].code[fr2[0]],nav);
                freq2=sat2freq(sat,obs[i].code[fr2[1]],nav);
                slip[i]=rtk->ssat[sat-1].slip[fr];
                if (nf==1||obs[i].P[fr2[0]]==0.0||obs[i].P[fr2[1]]==0.0||freq1==0.0||freq2==0.0)
                    ion=0;
                else
                    ion=(obs[i].P[fr2[0]]-obs[i].P[fr2[1]])/(1.0-SQR(freq1/freq2));
                bias[i]=L[f]-P[f]+2.0*ion*SQR(freq1/freq);
            }
            if (rtk->x[j]==0.0||slip[i]||bias[i]==0.0) continue;

            offset+=bias[i]-rtk->x[j];
            k++;
        }
        /* correct phase-code jump to ensure phase-code coherence */
        if (k>=2&&fabs(offset/k)>0.0005*CLIGHT) {
            for (i=0;i<MAXSAT;i++) {
                j=IB(i+1,f,&rtk->opt);
                if (rtk->x[j]!=0.0) rtk->x[j]+=offset/k;
            }
            trace(8,"phase-code jump corrected: %s n=%2d dt=%12.9fs\n",
                  time_str(rtk->sol.time,0),k,offset/k/CLIGHT);
        }
        for (i=0;i<n&&i<MAXOBS;i++) {
            sat=obs[i].sat;
            j=IB(sat,f,&rtk->opt);

            rtk->P[j+j*rtk->nx]+=SQR(rtk->opt.prn[0])*fabs(rtk->tt);

            if (bias[i]==0.0||(rtk->x[j]!=0.0&&!slip[i])) continue;

            /* reinitialize phase-bias if detecting cycle slip */
            initx(rtk,bias[i],VAR_BIAS,IB(sat,f,&rtk->opt));

            /* reset fix flags */
            for (k=0;k<MAXSAT;k++) rtk->ambc[sat-1].flags[k]=0;

            trace(8,"udbias_ppp: sat=%2d bias=%.3f\n",sat,bias[i]);
        }
    }
}
/* temporal update of states --------------------------------------------------*/
static void udstate_ppp(rtk_t *rtk, const obsd_t *obs, int n, const nav_t *nav)
{
    trace(3,"udstate_ppp: n=%d\n",n);

    /* temporal update of position */
    udpos_ppp(rtk);

    /* temporal update of clock */
    udclk_ppp(rtk);

    /* temporal update of tropospheric parameters */
    if (rtk->opt.tropopt==TROPOPT_EST||rtk->opt.tropopt==TROPOPT_ESTG) {
        udtrop_ppp(rtk);
    }
    /* temporal update of ionospheric parameters */
    if (rtk->opt.ionoopt==IONOOPT_EST) {
        udiono_ppp(rtk,obs,n,nav);
    }
    /* temporal update of L5-receiver-dcb parameters */
    if (rtk->opt.nf>=3) {
        uddcb_ppp(rtk);
    }
    /* temporal update of phase-bias */
    udbias_ppp(rtk,obs,n,nav);
}
/* satellite antenna phase center variation ----------------------------------*/
static void satantpcv(const double *rs, const double *rr, const spcv_t *spcv,
                      double *dant)
{
    double ru[3],rz[3],eu[3],ez[3],nadir,cosa;
    int i;

    for (i=0;i<3;i++) {
        ru[i]=rr[i]-rs[i];
        rz[i]=-rs[i];
    }
    if (!normv3(ru,eu)||!normv3(rz,ez)) return;

    cosa=dot3(eu,ez);
    cosa=cosa<-1.0?-1.0:(cosa>1.0?1.0:cosa);
    nadir=acos(cosa);

    antmodel_s(spcv,nadir,dant);
}
/* precise tropospheric model ------------------------------------------------*/
static double trop_model_prec(gtime_t time, const double *pos,
                              const double *azel, const double *x, double *dtdx,
                              double *var)
{
    const double zazel[]={0.0,PI/2.0};
    double zhd,m_h,m_w,cotz,grad_n,grad_e;

    /* zenith hydrostatic delay */
    zhd=tropmodel(time,pos,zazel,0.0);

    /* mapping function */
    m_h=tropmapf(time,pos,azel,&m_w);

    if (azel[1]>0.0) {

        /* m_w=m_0+m_0*cot(el)*(Gn*cos(az)+Ge*sin(az)): ref [6] */
        cotz=1.0/tan(azel[1]);
        grad_n=m_w*cotz*cos(azel[0]);
        grad_e=m_w*cotz*sin(azel[0]);
        m_w+=grad_n*x[1]+grad_e*x[2];
        dtdx[1]=grad_n*(x[0]-zhd);
        dtdx[2]=grad_e*(x[0]-zhd);
    }
    dtdx[0]=m_w;
    *var=SQR(0.01);
    return m_h*zhd+m_w*(x[0]-zhd);
}
/* tropospheric model ---------------------------------------------------------*/
static int model_trop(gtime_t time, const double *pos, const double *azel,
                      const prcopt_t *opt, const double *x, double *dtdx,
                      const nav_t *nav, double *dtrp, double *var)
{
    double trp[3]={0};

    if (opt->tropopt==TROPOPT_SAAS) {
        *dtrp=tropmodel(time,pos,azel,REL_HUMI);
        *var=SQR(ERR_SAAS);
        return 1;
    }
    if (opt->tropopt==TROPOPT_SBAS) {
        *dtrp=sbstropcorr(time,pos,azel,var);
        return 1;
    }
    if (opt->tropopt==TROPOPT_EST||opt->tropopt==TROPOPT_ESTG) {
        matcpy(trp,x+IT(opt),opt->tropopt==TROPOPT_EST?1:3,1);
        *dtrp=trop_model_prec(time,pos,azel,trp,dtdx,var);
        return 1;
    }
    return 0;
}
/* ionospheric model ---------------------------------------------------------*/
static int model_iono(gtime_t time, const double *pos, const double *azel,
                      const prcopt_t *opt, int sat, const double *x,
                      const nav_t *nav, double *dion, double *var)
{
    if (opt->ionoopt==IONOOPT_SBAS) {
        return sbsioncorr(time,nav,pos,azel,dion,var);
    }
    if (opt->ionoopt==IONOOPT_TEC) {
        return iontec(time,nav,pos,azel,1,dion,var);
    }
    if (opt->ionoopt==IONOOPT_BRDC) {
        *dion=ionmodel(time,nav->ion_gps,pos,azel);
        *var=SQR(*dion*ERR_BRDCI);
        return 1;
    }
    if (opt->ionoopt==IONOOPT_EST) {
        /* Estimated delay is a vertical delay, apply the mapping function. */
        /* *dion=x[II(sat,opt)]*ionmapf(pos,azel); */
        *dion=x[II(sat,opt)];
        *var=0.0;
        return 1;
    }
    if (opt->ionoopt==IONOOPT_IFLC) {
        *dion=*var=0.0;
        return 1;
    }
    return 0;
}
/* phase and code residuals --------------------------------------------------*/
static int ppp_res(int post, const obsd_t *obs, int n, const double *rs,
                   const double *dts, const double *var_rs, const int *svh,
                   const double *dr, int *exc, const nav_t *nav,
                   const double *x, rtk_t *rtk, double *v, double *H, double *R,
                   double *azel)
{
    prcopt_t *opt=&rtk->opt;
    double y,r,cdtr,bias,rr[3],pos[3],e[3],dtdx[3],L[NFREQ],P[NFREQ],Lc,Pc,C,fact;
    double var[MAXOBS*2],dtrp=0.0,dion=0.0,vart=0.0,vari=0.0,dcb,freq,res=0.0;
    double dantr[NFREQ]={0},dants[NFREQ]={0};
    double ve[MAXOBS*2*NFREQ]={0},vmax=0;
    char str[32],id[4];
    int ne=0,obsi[MAXOBS*2*NFREQ]={0},frqi[MAXOBS*2*NFREQ],maxobs,maxfrq,rej;
    int i,j,k,sat,sys,nv=0,nx=rtk->nx,stat=1,frq,code,fr;

    /* if broadcast ephemeris is used, enlarge the residual threshold */
    fact=EPHOPT_PREC==opt->sateph?1.0:2.0;

    time2str(obs[0].time,str,2);

    for (i=0;i<MAXSAT;i++) {
        sys=satsys(i+1,NULL);

        for (j=0;j<opt->nf;j++) {
            fr=sys2freid(sys,j,opt);
            rtk->ssat[i].vsat[fr]=0;
        }
    }

    for (i=0;i<3;i++) rr[i]=x[i]+dr[i];
    ecef2pos(rr,pos);

    for (i=0;i<n&&i<MAXOBS;i++) {
        sat=obs[i].sat;
        satno2id(sat,id);
        sys=satsys(sat,NULL);

        if ((r=geodist(rs+i*6,rr,e))<=0.0||
            satazel(pos,e,azel+i*2)<opt->elmin) {
            exc[i]=1;
            continue;
        }
        if (!(sys=satsys(sat,NULL))||!rtk->ssat[sat-1].vs||
            satexclude(sat,var_rs[i],svh[i],opt)||exc[i]) {
            exc[i]=1;
            continue;
        }
        /* tropospheric and ionospheric model */
        if (!model_trop(obs[i].time,pos,azel+i*2,opt,x,dtdx,nav,&dtrp,&vart)||
            !model_iono(obs[i].time,pos,azel+i*2,opt,sat,x,nav,&dion,&vari)) {
            continue;
        }
        /* satellite and receiver antenna model */
        if (opt->posopt[0]) satantpcv(rs+i*6,rr,nav->spcvs+sat-1,dants);
        antmodel(sys,opt->pcvr,opt->antdel[0],azel+i*2,opt->posopt[1],dantr);

        /* phase windup model */
        if (!model_phw(rtk->sol.time,sat,nav->spcvs[sat-1].type,
                       opt->posopt[2]?2:0,rs+i*6,rr,&rtk->ssat[sat-1].phw)) {
            continue;
        }
        /* corrected phase and code measurements */
        corr_meas(obs+i,nav,azel+i*2,&rtk->opt,dantr,dants,
                  rtk->ssat[sat-1].phw,L,P,&Lc,&Pc);

        /* stack phase and code residuals {L1,P1,L2,P2,...} */
        for (j=0;j<2*NF(opt);j++) {
            C=0.0;

            dcb=bias=0.0;
            code=j%2; /* 0=phase, 1=code */
            frq=j/2;
            fr=sys2freid(sys,frq,opt);

            if (opt->ionoopt==IONOOPT_IFLC) {
                if ((y=code==0?Lc:Pc)==0.0) continue;
            }
            else {
                if ((y=code==0?L[frq]:P[frq])==0.0||fabs(P[frq]-P[0])>1000) {
                    trace(6,"The code/phase observation is missing or invalid. sat=%s, %s%d\n",id,code?"P":"L",fr+1);
                    continue; 
                }

                if ((freq=sat2freq(sat,obs[i].code[fr],nav))==0.0) continue;
                /* The iono paths have already applied a slant factor, based on GPS L1 frequency*/
                C=SQR(FREQL1/freq)*(code==0?-1.0:1.0);
            }
            for (k=0;k<nx;k++) {
                H[k+nx*nv]=k<3?-e[k]:0.0;               
            }

            /* receiver clock, if only use a system (no GPS), x[IC(0,opt)]=0*/
            if (sys==SYS_GPS) {
				cdtr=x[IC(0,opt)];
				H[IC(0,opt)+nx*nv]=1.0;
			}
            if (sys==SYS_CMP) {
				cdtr=x[IC(0,opt)]+x[IC(3,opt)];
				H[IC(0,opt)+nx*nv]=1.0;
				H[IC(3,opt)+nx*nv]=1.0;
			}
			if (sys==SYS_GAL) {
				cdtr=x[IC(0,opt)]+x[IC(2,opt)];
				H[IC(0,opt)+nx*nv]=1.0;
				H[IC(2,opt)+nx*nv]=1.0;
			}
			if (sys==SYS_IRN) {
				cdtr=x[IC(0,opt)]+x[IC(4,opt)];
				H[IC(0,opt)+nx*nv]=1.0;
				H[IC(4,opt)+nx*nv]=1.0;
			}
            if (sys==SYS_QZS) {
				cdtr=x[IC(0,opt)]+x[IC(5,opt)];
				H[IC(0,opt)+nx*nv]=1.0;
				H[IC(5,opt)+nx*nv]=1.0;
			}

            if (opt->tropopt==TROPOPT_EST||opt->tropopt==TROPOPT_ESTG) {
                for (k=0;k<(opt->tropopt>=TROPOPT_ESTG?3:1);k++) {
                    H[IT(opt)+k+nx*nv]=dtdx[k];
                }
            }
            if (opt->ionoopt==IONOOPT_EST) {
                if (rtk->x[II(sat,opt)]==0.0) continue;
                /* The vertical iono delay is estimated, but the residual is
                 * in the direction of the slant, so apply the slat factor
                 * mapping function. */
                /* H[II(sat,opt)+nx*nv]=C*ionmapf(pos,azel+i*2); */
                H[II(sat,opt)+nx*nv]=C;
            }
            if (frq==2&&code==1) { /* L5-receiver-dcb */
                dcb+=rtk->x[ID(opt)];
                H[ID(opt)+nx*nv]=1.0;
            }
            if (code==0) { /* phase bias */
                if ((bias=x[IB(sat,frq,opt)])==0.0) continue;
                H[IB(sat,frq,opt)+nx*nv]=1.0;
            }
            /* residual */
            res=y-(r+cdtr-CLIGHT*dts[i*2]+dtrp+C*dion+dcb+bias);
            if (v) v[nv]=res;

            if (code==0) rtk->ssat[sat-1].resc[fr]=res;  /* carrier phase */
            else         rtk->ssat[sat-1].resp[fr]=res;  /* pseudorange */

            /* variance */
            var[nv]=varerr(sat,sys,azel[1+i*2],SNR_UNIT*rtk->ssat[sat-1].snr_rover[fr],j,opt,obs+i);
            var[nv]+=vart+SQR(C)*vari+var_rs[i];
            if (sys==SYS_GLO&&code==1) var[nv]+=VAR_GLO_IFB;

            trace(8,"%4s sat=%s %s%d res=%9.4f sig=%9.4f el=%4.1f\n",post?"post":"prio",id,code?"P":"L",
                fr+1,res,sqrt(var[nv]),azel[1+i*2]*R2D);

            /* reject satellite by pre-fit residuals */
            if (!post&&opt->maxinno[code]>0.0&&fabs(res)>(opt->maxinno[code]*fact)) {
                trace(7,"(%4s) outlier rejected sat=%s %s%d, res=%9.4f, thres=%9.4f, el=%4.1f\n",post?"post":"prio",id,code?"P":"L",
                    fr+1,res,opt->maxinno[code],azel[1+i*2]*R2D);
                exc[i]=1; rtk->ssat[sat-1].rejc[fr]++;
                continue;
            }
            /* record large post-fit residuals */
            if (post&&fabs(res)>sqrt(var[nv])*THRES_REJECT) {
                obsi[ne]=i; frqi[ne]=fr; ve[ne]=res; ne++;
            }
            if (code==0) rtk->ssat[sat-1].vsat[fr]=1;
            nv++;
        }
    }
    /* reject satellite with large and max post-fit residual */
    if (post&&ne>0) {
        vmax=ve[0]; maxobs=obsi[0]; maxfrq=frqi[0]; rej=0;
        for (j=1;j<ne;j++) {
            if (fabs(vmax)>=fabs(ve[j])) continue;
            vmax=ve[j]; maxobs=obsi[j]; maxfrq=frqi[j]; rej=j;
        }
        sat=obs[maxobs].sat;
        trace(2,"outlier (%d) rejected %s sat=%2d %s%d res=%9.4f el=%4.1f\n",
            post,str,sat,maxfrq%2?"P":"L",maxfrq/2+1,vmax,azel[1+maxobs*2]*R2D);
        exc[maxobs]=1; rtk->ssat[sat-1].rejc[maxfrq]++; stat=0;
        ve[rej]=0;
    }
    if (R) {
        for (j=0;j<nv;j++) for (i=0;i<nv;i++) R[i+j*nv]=0.0;
        for (i=0;i<nv;i++) R[i+i*nv]=var[i];
    }
    return post?stat:nv;
}
/* number of estimated states ------------------------------------------------*/
extern int pppnx(const prcopt_t *opt)
{
    return NX(opt);
}
/* update solution status ----------------------------------------------------*/
static void update_stat(rtk_t *rtk, const obsd_t *obs, int n, int stat)
{
    const prcopt_t *opt=&rtk->opt;
    int i,j,sys,fr;

    /* test # of valid satellites */
    rtk->sol.ns=0;
    for (i=0;i<n&&i<MAXOBS;i++) {
        sys=satsys(obs[i].sat,NULL);
        for (j=0;j<opt->nf;j++) {
            fr=sys2freid(sys,j,opt);

            if (!rtk->ssat[obs[i].sat-1].vsat[fr]) continue;
            rtk->ssat[obs[i].sat-1].lock[fr]++;
            rtk->ssat[obs[i].sat-1].outc[fr]=0;
            if (j==0) rtk->sol.ns++;
        }
    }
    /*posterior result check*/
    rtk->sol.stat=rtk->sol.ns<MIN_NSAT_SOL?SOLQ_NONE:stat;

    if (rtk->sol.stat==SOLQ_FIX) {
        for (i=0;i<3;i++) {
            rtk->sol.rr[i]=rtk->xa[i];
            rtk->sol.qr[i]=(float)rtk->Pa[i+i*rtk->na];
        }
        rtk->sol.qr[3]=(float)rtk->Pa[1];
        rtk->sol.qr[4]=(float)rtk->Pa[1+2*rtk->na];
        rtk->sol.qr[5]=(float)rtk->Pa[2];
    }
    else {
        for (i=0;i<3;i++) {
            rtk->sol.rr[i]=rtk->x[i];
            rtk->sol.qr[i]=(float)rtk->P[i+i*rtk->nx];
        }
        rtk->sol.qr[3]=(float)rtk->P[1];
        rtk->sol.qr[4]=(float)rtk->P[2+rtk->nx];
        rtk->sol.qr[5]=(float)rtk->P[2];

        if (rtk->opt.dynamics) { /* velocity and covariance */
            for (i=3;i<6;i++) {
                rtk->sol.rr[i]=rtk->x[i];
                rtk->sol.qv[i-3]=(float)rtk->P[i+i*rtk->nx];
            }
            rtk->sol.qv[3]=(float)rtk->P[4+3*rtk->nx];
            rtk->sol.qv[4]=(float)rtk->P[5+4*rtk->nx];
            rtk->sol.qv[5]=(float)rtk->P[5+3*rtk->nx];
        }
    }
        rtk->sol.dtr[0]=rtk->x[IC(0,opt)]; /* GPS */
        rtk->sol.dtr[1]=rtk->x[IC(1,opt)]-rtk->x[IC(0,opt)]; /* GLO-GPS */
        rtk->sol.dtr[2]=rtk->x[IC(2,opt)]-rtk->x[IC(0,opt)]; /* GAL-GPS */
        rtk->sol.dtr[3]=rtk->x[IC(3,opt)]-rtk->x[IC(0,opt)]; /* BDS-GPS */

    for (i=0;i<n&&i<MAXOBS;i++) {
        sys=satsys(obs[i].sat,NULL);

        for (j=0;j<opt->nf;j++) {
            fr=sys2freid(sys,j,opt);
            rtk->ssat[obs[i].sat-1].snr_rover[fr]=obs[i].SNR[fr];
            rtk->ssat[obs[i].sat-1].snr_base[fr] =0;
        }
    }
    for (i=0;i<MAXSAT;i++) {
        sys=satsys(i+1,NULL);

        for (j=0;j<opt->nf;j++) {
            fr=sys2freid(sys,j,opt);
            if (rtk->ssat[i].slip[fr]&3) rtk->ssat[i].slipc[fr]++;
            if (rtk->ssat[i].fix[fr]==2&&stat!=SOLQ_FIX) rtk->ssat[i].fix[fr]=1;
    }
    }
}
/* test hold ambiguity -------------------------------------------------------*/
static int test_hold_amb(rtk_t *rtk)
{
    prcopt_t *opt=&rtk->opt;
    int i,j,stat=0,sys,fr;

    /* no fix-and-hold mode */
    if (rtk->opt.modear!=ARMODE_FIXHOLD) return 0;

    /* reset # of continuous fixed if new ambiguity introduced */
    for (i=0;i<MAXSAT;i++) {
        if (rtk->ssat[i].fix[0]!=2&&rtk->ssat[i].fix[1]!=2) continue;
        for (j=0;j<MAXSAT;j++) {
            if (rtk->ssat[j].fix[0]!=2&&rtk->ssat[j].fix[1]!=2) continue;
            if (!rtk->ambc[j].flags[i]||!rtk->ambc[i].flags[j]) stat=1;
            rtk->ambc[j].flags[i]=rtk->ambc[i].flags[j]=1;
        }
    }
    if (stat) {
        rtk->nfix=0;
        return 0;
    }
    /* test # of continuous fixed */
    return ++rtk->nfix>=rtk->opt.minfix;
}

/* PPP observation value pre-check*/
extern void obsScan_PPP(const prcopt_t *opt, obsd_t *obs, const int nobs, int *ns)
{
	int i,n,sat,sys,fr2[2];

	for (i=n=0;i<nobs&&i<MAXOBS;i++) {
		sat=obs[i].sat;
        sys=satsys(sat,NULL);
        fr2[0]=sys2freid(sys,0,opt);
        fr2[1]=sys2freid(sys,1,opt);

        if (opt->mode>=PMODE_PPP_KINEMA) {
            if ((fabs(obs[i].L[fr2[0]])==0.0)&&(fabs(obs[i].L[fr2[1]])==0.0)) continue;
        }

        if (fabs(obs[i].P[fr2[0]]-obs[i].P[fr2[1]])>=200.0) continue;

        obs[n]=obs[i];
        n++;   
	}

	if (ns) *ns=n;
}

/* precise point positioning -------------------------------------------------*/
extern void pppos(rtk_t *rtk, const obsd_t *obs, int n, const nav_t *nav)
{
    const prcopt_t *opt=&rtk->opt;
    double *rs,*dts,*var,*v,*H,*R,*azel,*xp,*Pp,dr[3]={0},std[3];
    char str[32];
    int i,j,nv,info,svh[MAXOBS],exc[MAXOBS]={0},stat=SOLQ_SINGLE,sys,fr;

    time2str(obs[0].time,str,2);
    trace(3,"pppos   : time=%s nx=%d n=%d\n",str,rtk->nx,n);

    rs=mat(6,n); dts=mat(2,n); var=mat(1,n); azel=zeros(2,n);

    for (i=0;i<MAXSAT;i++) {
        sys=satsys(i+1,NULL);
        for (j=0;j<opt->nf;j++) {
            fr=sys2freid(sys,j,opt);
            rtk->ssat[i].fix[fr]=0;
        }
    }
    for (i=0;i<n&&i<MAXOBS;i++) {
        sys=satsys(obs[i].sat,NULL);
        for (j=0;j<opt->nf;j++) {
            fr=sys2freid(sys,j,opt);
            rtk->ssat[obs[i].sat-1].snr_rover[fr]=obs[i].SNR[fr];
            rtk->ssat[obs[i].sat-1].snr_base[fr] =0;
        }  
    }

    /* temporal update of ekf states */
    udstate_ppp(rtk,obs,n,nav);

    /* satellite positions and clocks */
    satposs(obs[0].time,obs,n,nav,rtk->opt.sateph,rs,dts,var,svh);

    /* exclude measurements of eclipsing satellite (block IIA) */
    if (rtk->opt.posopt[3]) {
        testeclipse(obs,n,nav,rs);
    }
    /* earth tides correction */
    if (opt->tidecorr) {
        tidedisp(gpst2utc(obs[0].time),rtk->x,opt->tidecorr==1?1:7,&nav->erp,
                 opt->odisp[0],dr);
    }
    nv=n*rtk->opt.nf*2+MAXSAT+3;
    xp=mat(rtk->nx,1); Pp=zeros(rtk->nx,rtk->nx);
    v=mat(nv,1); H=mat(rtk->nx,nv); R=mat(nv,nv);

    for (i=0;i<MAX_ITER;i++) {

        matcpy(xp,rtk->x,rtk->nx,1);
        matcpy(Pp,rtk->P,rtk->nx,rtk->nx);

        /* prefit residuals */
        if (!(nv=ppp_res(0,obs,n,rs,dts,var,svh,dr,exc,nav,xp,rtk,v,H,R,azel))) {
            trace(2,"%s ppp (%d) no valid obs data\n",str,i+1);
            break;
        }
        /* measurement update of ekf states */
        if ((info=filter(xp,Pp,H,v,R,rtk->nx,nv))) {
            trace(2,"%s ppp (%d) filter error info=%d\n",str,i+1,info);
            break;
        }
        /* postfit residuals */
        if (ppp_res(i+1,obs,n,rs,dts,var,svh,dr,exc,nav,xp,rtk,v,H,R,azel)) {
            matcpy(rtk->x,xp,rtk->nx,1);
            matcpy(rtk->P,Pp,rtk->nx,rtk->nx);
            stat=SOLQ_PPP;
            break;
        }
    }
    if (i>=MAX_ITER) {
        trace(2,"%s ppp (%d) iteration overflows\n",str,i);
    }
    if (stat==SOLQ_PPP) {

        if (ppp_ar(rtk,obs,n,exc,nav,azel,xp,Pp)&&
            ppp_res(9,obs,n,rs,dts,var,svh,dr,exc,nav,xp,rtk,v,H,R,azel)) {

            matcpy(rtk->xa,xp,rtk->nx,1);
            matcpy(rtk->Pa,Pp,rtk->nx,rtk->nx);

            for (i=0;i<3;i++) std[i]=sqrt(Pp[i+i*rtk->nx]);
            if (norm(std,3)<MAX_STD_FIX) stat=SOLQ_FIX;
        }
        else {
            rtk->nfix=0;
        }
        /* update solution status */
        update_stat(rtk,obs,n,stat);

        if (stat==SOLQ_FIX&&test_hold_amb(rtk)) {
            matcpy(rtk->x,xp,rtk->nx,1);
            matcpy(rtk->P,Pp,rtk->nx,rtk->nx);
            trace(2,"%s hold ambiguity\n",str);
            rtk->nfix=0;
        }
    }
    free(rs); free(dts); free(var); free(azel);
    free(xp); free(Pp); free(v); free(H); free(R);
}
