// Omni Sampler 2.4.37: the Roland MV-8800 MFX (30 effects of hangnef's MV-8800 mod 4.1: MC-909, V-Synth XT and SP-505
// effects), the user's OmniMV8800FX.cpp (XTRA/MV8800_4.zip, 2026-11-24). The parameter tables are the .MVE files' own
// (names, ranges, defaults: all 30 .MVE defaults = these initial values); the DSP is the user's own model of each effect.
// Omni changes (staging-2437/OmniMV8800FX_opt.cpp, output within -92.8 dB of the original): sine / cosine from a table,
// each filter stage's tan() cached per frequency, dB of integer controls from a table, exp2 caches, delay taps without
// a division; prepare() / load() / reset() for the FX BANK bay (no allocation on the audio thread); TimeSkip Dly's
// delay glide reads its Accel (control 3; the original read Feedbk). The .MVE loader / C API are left out (unused).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
namespace omnisampler {
namespace mv8800 {
struct Control { const char* name; int minimum,maximum,initial; };
struct Program { const char* name; unsigned count; Control controls[28]; };
enum Effect {
DUALPITCH, AUTOPAN, BASSMLTI, EPMULTI, FBKPITCH, GRAPHEQ, GTRAMP, HEXACHOR, HUMANIZR, KBDMULTI, LOFINOIS, MODDELAY, MULTITAP, OCTAVE, QUADTAP, REVDELAY, RINGMOD, ROTARY, SHUFFLE, SLCRFLNG, SLICER, SPECTRUM, STEPFLNG, STEPFLT, STEPPHSR, SUPFILT, TIMECTRL, TIMESKIP, TREMOLO, TRMCHORS};
constexpr Program programs[]={
{"2Vo Pitch",12,{
{"Grade",1,5,3},
{"Crs A",-24,12,7},
{"Fine A",-50,50,0},
{"PreDlA",0,125,0},
{"Pan A",-64,63,-64},
{"Crs B",-24,12,-5},
{"Fine B",-50,50,0},
{"PreDlB",0,125,0},
{"Pan B",-64,63,63},
{"AB Bal",0,100,50},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"Auto Pan",6,{
{"Wave",0,4,2},
{"Rate",0,200,40},
{"Depth",0,127,100},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Level",0,127,127},
}},
{"Bass Multi",28,{
{"CompSw",0,1,1},
{"CmpThr",-60,0,-20},
{"CmpAtk",0,127,20},
{"CmpRel",0,127,64},
{"Ratio",0,3,1},
{"CmpGn",-60,12,6},
{"DrvSw",0,1,1},
{"DrvMd",0,1,0},
{"Drive",0,127,64},
{"Amp Sw",0,1,1},
{"AmpTyp",0,3,1},
{"DistLv",0,127,100},
{"LoFreq",0,19,3},
{"LoGain",-15,15,0},
{"MdFreq",0,26,13},
{"Mid Q",0,5,2},
{"MdGain",-15,15,0},
{"HiFreq",0,8,2},
{"HiGain",-15,15,0},
{"ModMd",0,1,0},
{"ModRt",0,111,20},
{"ModDep",0,127,40},
{"ModPhs",0,90,90},
{"ModDly",0,100,10},
{"ModFb",-49,49,0},
{"ModLPF",0,16,16},
{"ModHPF",0,13,0},
{"ModLv",0,127,100},
}},
{"EP Multi",24,{
{"Enh Sw",0,1,1},
{"EnhSns",0,127,64},
{"EnhFrq",0,127,64},
{"EnhMix",0,127,64},
{"EnhLv",0,127,127},
{"PhMan",0,127,64},
{"PhRt",0,111,20},
{"PhDep",0,127,64},
{"PhReso",0,127,40},
{"PhMix",0,127,100},
{"ModMd",0,1,0},
{"ModRt",0,111,20},
{"ModDep",0,127,40},
{"ModPhs",0,90,90},
{"ModDly",0,100,10},
{"ModFb",-49,49,0},
{"ModLPF",0,16,16},
{"ModHPF",0,13,0},
{"ModLv",0,127,100},
{"TremSw",0,1,1},
{"TrmMd",0,1,1},
{"TrmWav",0,4,4},
{"TrmRt",0,111,40},
{"TrmDep",0,127,80},
}},
{"Fbk Pitch",10,{
{"Grade",1,5,3},
{"Coarse",-24,12,7},
{"Fine",-50,50,0},
{"PreDly",0,125,0},
{"Feedbk",-49,49,20},
{"Pan",-64,63,0},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"Graphic EQ",13,{
{"180Hz",-15,15,0},
{"250Hz",-15,15,0},
{"355Hz",-15,15,0},
{"500Hz",-15,15,0},
{"710Hz",-15,15,0},
{"1kHz",-15,15,0},
{"1.4kHz",-15,15,0},
{"2kHz",-15,15,0},
{"2.8kHz",-15,15,0},
{"4kHz",-15,15,0},
{"5.6kHz",-15,15,0},
{"8kHz",-15,15,0},
{"Total",-15,15,0},
}},
{"Gtr Amp Sim",17,{
{"Amp Sw",0,1,1},
{"AmpTyp",0,13,3},
{"Volume",0,127,80},
{"Master",0,127,100},
{"Gain",0,2,1},
{"Bass",0,127,64},
{"Middle",0,127,64},
{"Treble",0,127,64},
{"Presnc",0,127,0},
{"Bright",0,1,0},
{"SpkSw",0,1,1},
{"SpkTyp",0,15,9},
{"MicSet",1,3,2},
{"MicLv",0,127,127},
{"DirLev",0,127,0},
{"Pan",-64,63,0},
{"Level",0,127,127},
}},
{"Hexa-Chorus",8,{
{"PreDly",0,125,20},
{"Rate",0,200,10},
{"Depth",0,127,64},
{"DlyDev",0,20,10},
{"DepDev",-20,20,0},
{"PanDev",0,20,20},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"Humanizer",13,{
{"DrvSw",0,1,0},
{"Drive",0,127,127},
{"Vowel1",0,4,0},
{"Vowel2",0,4,2},
{"Rate",0,200,20},
{"Depth",0,127,127},
{"SyncSw",0,1,0},
{"SyncTh",0,127,60},
{"Manual",0,100,50},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Pan",-64,63,0},
{"Level",0,127,127},
}},
{"Kbd Multi",25,{
{"RngFrq",0,127,64},
{"RngBal",0,100,50},
{"LoFreq",0,19,3},
{"LoGain",-15,15,0},
{"MdFreq",0,26,13},
{"Mid Q",0,5,2},
{"MdGain",-15,15,0},
{"HiFreq",0,8,2},
{"HiGain",-15,15,0},
{"PSGrad",1,5,4},
{"PS Crs",-12,12,7},
{"PSFine",-50,50,0},
{"PS Bal",0,100,30},
{"PhMan",0,127,64},
{"PhRt",0,111,20},
{"PhDep",0,127,64},
{"PhReso",0,127,40},
{"PhMix",0,127,100},
{"DlyTm",0,105,60},
{"DlyFb",-49,49,16},
{"LDFreq",0,19,0},
{"LDGain",-36,0,0},
{"HDFreq",0,8,8},
{"HDGain",-36,0,0},
{"DlyLv",0,127,80},
}},
{"LoFi Noise",14,{
{"LoFiTp",0,8,2},
{"PstFlt",0,2,0},
{"PstCut",0,16,8},
{"W/P Tp",0,1,0},
{"WP LPF",0,17,17},
{"W/P Lv",0,127,0},
{"DiscTp",0,3,0},
{"DscLPF",0,17,17},
{"DiscLv",0,127,0},
{"Hum Tp",0,1,0},
{"HumLPF",0,17,17},
{"Hum Lv",0,127,0},
{"Dir/FX",0,100,100},
{"Level",0,127,127},
}},
{"Mod Delay",12,{
{"FbMode",0,1,0},
{"Dly L",0,2000,300},
{"Dly R",0,2000,450},
{"Feedbk",-49,49,20},
{"HFDamp",0,17,17},
{"Rate",0,200,20},
{"Depth",0,127,40},
{"Phase",0,90,90},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"MultiTap Dly",18,{
{"Dly 1",0,4000,250},
{"Dly 2",0,4000,500},
{"Dly 3",0,4000,750},
{"Dly 4",0,4000,1000},
{"Pan 1",-64,63,-64},
{"Pan 2",-64,63,-24},
{"Pan 3",-64,63,24},
{"Pan 4",-64,63,63},
{"Lev 1",0,127,127},
{"Lev 2",0,127,110},
{"Lev 3",0,127,90},
{"Lev 4",0,127,70},
{"Feedbk",-49,49,20},
{"HFDamp",0,17,17},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"Octave",3,{
{"-2OctLv",0,127,0},
{"-1OctLv",0,127,100},
{"DirLev",0,127,100},
}},
{"Quad Tap Dly",12,{
{"Dly 1",0,4000,250},
{"Dly 2",0,4000,500},
{"Dly 3",0,4000,750},
{"Dly 4",0,4000,1000},
{"Lev 1",0,127,127},
{"Lev 2",0,127,100},
{"Lev 3",0,127,80},
{"Lev 4",0,127,60},
{"Feedbk",-49,49,20},
{"HFDamp",0,17,17},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"Reverse Dly",19,{
{"Thresh",0,127,30},
{"Dly 1",0,2000,500},
{"Dly 2",0,2000,250},
{"Dly 3",0,2000,500},
{"Dly 4",0,2000,750},
{"Fdbk 1",-49,49,15},
{"Fdbk 4",-49,49,0},
{"HFDmp1",0,17,17},
{"HFDmp4",0,17,17},
{"Pan 1",-64,63,-64},
{"Pan 2",-64,63,63},
{"Pan 3",-64,63,0},
{"Lev 1",0,127,127},
{"Lev 2",0,127,100},
{"Lev 3",0,127,80},
{"Dir/FX",0,100,50},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Level",0,127,127},
}},
{"Ring Mod",7,{
{"Freq",0,127,64},
{"Sens",0,127,0},
{"Polar",0,1,1},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Dir/FX",0,127,127},
{"Level",0,127,127},
}},
{"Rotary",11,{
{"TwSlow",0,200,16},
{"WfSlow",0,200,14},
{"TwFast",0,200,134},
{"WfFast",0,200,114},
{"Speed",0,1,0},
{"TwAcc",0,15,12},
{"WfAcc",0,15,8},
{"TwLv",0,127,127},
{"WfLv",0,127,127},
{"Separ",0,127,96},
{"Level",0,127,127},
}},
{"Shuffle Dly",12,{
{"Delay",0,4000,600},
{"ShufRt",0,100,50},
{"Accel",0,15,10},
{"Pan A",-64,63,-64},
{"Pan B",-64,63,63},
{"AB Bal",0,100,50},
{"Feedbk",-49,49,20},
{"HFDamp",0,17,17},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"Slicer+Flngr",6,{
{"Ptn",1,16,1},
{"Rate",1,4,1},
{"Tempo",40,300,120},
{"TmpFin",0,99,0},
{"Run Sw",0,1,1},
{"Flange",0,127,64},
}},
{"Slicer",23,{
{"Step1",0,127,127},
{"Step2",0,127,0},
{"Step3",0,127,127},
{"Step4",0,127,0},
{"Step5",0,127,127},
{"Step6",0,127,127},
{"Step7",0,127,0},
{"Step8",0,127,127},
{"Step9",0,127,127},
{"Step10",0,127,0},
{"Step11",0,127,127},
{"Step12",0,127,0},
{"Step13",0,127,127},
{"Step14",0,127,127},
{"Step15",0,127,0},
{"Step16",0,127,127},
{"Rate",0,200,10},
{"Attack",0,127,20},
{"SyncSw",0,1,0},
{"SyncTh",0,127,60},
{"Mode",0,1,0},
{"Shuffl",0,127,0},
{"Level",0,127,127},
}},
{"Spectrum",11,{
{"250Hz",-15,15,0},
{"500Hz",-15,15,0},
{"1kHz",-15,15,0},
{"1.25k",-15,15,0},
{"2kHz",-15,15,0},
{"3.15k",-15,15,0},
{"4kHz",-15,15,0},
{"8kHz",-15,15,0},
{"Q",0,4,1},
{"Pan",-64,63,0},
{"Level",0,127,127},
}},
{"Step Flanger",10,{
{"PreDly",0,125,5},
{"Rate",0,200,10},
{"Depth",0,127,80},
{"Feedbk",-49,49,30},
{"StepRt",0,200,80},
{"Phase",0,90,90},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"Step Fltr",23,{
{"Step1",0,127,0},
{"Step2",0,127,32},
{"Step3",0,127,64},
{"Step4",0,127,96},
{"Step5",0,127,127},
{"Step6",0,127,96},
{"Step7",0,127,64},
{"Step8",0,127,32},
{"Step9",0,127,0},
{"Step10",0,127,32},
{"Step11",0,127,64},
{"Step12",0,127,96},
{"Step13",0,127,127},
{"Step14",0,127,96},
{"Step15",0,127,64},
{"Step16",0,127,32},
{"Rate",0,200,60},
{"Attack",0,127,50},
{"Type",0,3,0},
{"Slope",0,2,1},
{"Reso",0,127,50},
{"Gain",0,12,0},
{"Level",0,127,127},
}},
{"Step Phaser",12,{
{"Mode",0,1,1},
{"Polar",0,1,1},
{"Rate",0,200,10},
{"Depth",0,127,64},
{"Manual",0,127,64},
{"Reso",0,127,64},
{"X Fdbk",-49,49,0},
{"StepRt",0,200,80},
{"Mix",0,127,127},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Level",0,127,127},
}},
{"Super Filter",11,{
{"Type",0,3,0},
{"Slope",0,2,2},
{"Cutoff",0,127,60},
{"Reso",0,127,40},
{"Gain",0,12,0},
{"Mod Sw",0,1,1},
{"Wave",0,4,0},
{"Rate",0,200,40},
{"Depth",0,127,80},
{"Attack",0,127,64},
{"Level",0,127,127},
}},
{"TimeCtrl Dly",9,{
{"Delay",0,4000,500},
{"Feedbk",-49,49,20},
{"Accel",0,15,10},
{"HFDamp",0,17,17},
{"Pan",-64,63,0},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"TimeSkip Dly",10,{
{"Delay",0,4000,500},
{"SkipRt",0,200,20},
{"Feedbk",-49,49,20},
{"Accel",0,15,10},
{"HFDamp",0,17,17},
{"Pan",-64,63,0},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
{"Tremolo",6,{
{"Wave",0,4,2},
{"Rate",0,200,80},
{"Depth",0,127,100},
{"LoGain",-15,15,0},
{"HiGain",-15,15,0},
{"Level",0,127,127},
}},
{"Trem Chorus",8,{
{"PreDly",0,125,20},
{"ChoRt",0,200,10},
{"ChoDep",0,127,60},
{"TrmRt",0,200,60},
{"TrmSep",0,127,64},
{"TrmPhs",0,90,90},
{"Dir/FX",0,100,50},
{"Level",0,127,127},
}},
};
constexpr double tau=6.283185307179586;
constexpr unsigned count=sizeof(programs)/sizeof(programs[0]);
inline double blend(double a,double b,double t){return a+(b-a)*std::clamp(t,0.,1.);}
// opt: dB of the (integer) controls from a table; other values computed
inline const std::array<double,129>& dbTable(){static const std::array<double,129> t=[]{std::array<double,129> a{};for(int i=0;i<129;++i)a[i]=std::exp2((i-64)/6.020599913);return a;}();return t;}
inline double db(double value){int i=int(value);if(double(i)==value&&i>=-64&&i<=64)return dbTable()[unsigned(i+64)];return std::exp2(value/6.020599913);}
inline double cycle(double phase){return phase-std::floor(phase);}
// opt: sine from a 4096-point table, linear interpolation (error < 3e-7)
inline const std::array<float,4097>& sineTable(){static const std::array<float,4097> t=[]{std::array<float,4097> a{};for(int i=0;i<=4096;++i)a[i]=float(std::sin(tau*i/4096.));return a;}();return t;}
inline double sine(double phase){double x=cycle(phase)*4096.;unsigned i=unsigned(x);if(i>4095)i=4095;double f=x-i;const auto&t=sineTable();return t[i]+(t[i+1]-t[i])*f;}
inline double cosine(double phase){return sine(phase+.25);}
struct Channel {
    std::vector<float> history;
    std::array<double,12> low{},band{},allpass{};
    std::array<double,12> fHz{},fG{},fQ{},fK{},fA{};   // opt: each stage's last frequency / Q, tan(), 1/Q and 1/(1+g(g+1/Q))
    double bass{},treble{},last{},envelope{},subSign{1},quarterSign{1};
    // 2.4.37: allocated in prepare() (4 s of history at the rate), cleared on every program change
    void prepare(unsigned hz){history.assign(std::size_t(hz)*4+4,0.f);clear();}
    void clear(){std::fill(history.begin(),history.end(),0.f);low.fill(0);band.fill(0);allpass.fill(0);fHz.fill(0);fG.fill(0);fQ.fill(0);fK.fill(0);fA.fill(0);
        bass=treble=last=envelope=0;subSign=quarterSign=1;}
    double filter(unsigned stage,double x,double hz,double q,double rate,int mode){
        double f=std::clamp(hz,10.,rate*.42);
        if(f!=fHz[stage]){fHz[stage]=f;fG[stage]=std::tan(tau*.5*f/rate);fQ[stage]=0;}
        if(q!=fQ[stage]){fQ[stage]=q;fK[stage]=1/q;fA[stage]=1/(1+fG[stage]*(fG[stage]+fK[stage]));}   // 2.4.37: no division per sample
        double g=fG[stage];
        double high=(x-(fK[stage]+g)*band[stage]-low[stage])*fA[stage];
        double bp=g*high+band[stage],lp=g*bp+low[stage];
        band[stage]=bp+g*high;low[stage]=lp+g*bp;
        return mode==0?lp:mode==1?bp:mode==2?high:lp+high;
    }
};
class Processor {
    Effect id=OCTAVE;
    double hz=44100;
    Channel channels[2];
    unsigned head=0;
    double position=0, delayMs=0;
    std::array<int,28> p{};
    std::uint32_t seed=777;
    int ringKey=-1;double ringHz=0;   // opt
    double tap(const Channel& c,double ms) const {
        double t=std::clamp(ms*hz*.001,1.,double(c.history.size()-2));
        unsigned n=unsigned(t),size=unsigned(c.history.size());
        unsigned a=head>=n?head-n:head+size-n;   // opt: no division (n <= size-2)
        unsigned a1=a?a-1:size-1;
        return blend(c.history[a],c.history[a1],t-n);
    }
    mutable double shiftKey[2]{1e9,1e9},shiftRatio[2]{};   // opt: exp2 of the last two shifts
    double ratio(double shift) const {
        for(int k=0;k<2;++k)if(shiftKey[k]==shift)return shiftRatio[k];
        shiftKey[1]=shiftKey[0];shiftRatio[1]=shiftRatio[0];shiftKey[0]=shift;return shiftRatio[0]=std::exp2(shift/12.);
    }
    double pitch(const Channel& c,double shift,double pre) const {
        if(std::abs(shift)<.00001)return tap(c,pre+.03);
        double a=cycle(position*(1-ratio(shift))/.05);
        double b=cycle(a+.5),weight=.5-.5*cosine(a);
        return blend(tap(c,pre+50*b),tap(c,pre+50*a),weight);
    }
    double phaser(Channel& c,double x,double phase,int stages){
        double a=.15+.68*(.5+.5*sine(phase));
        for(int i=0;i<stages;++i){double y=c.allpass[i]-a*x;c.allpass[i]=x+a*y;x=y;}
        return x;
    }
    double frame(Channel& c,unsigned side,double dry,double bpm){
        double x=dry,feed=0,wet=1,level=1;
        auto u=[&](unsigned i){return p[i]*(1/127.);};   // 2.4.37: a product (no division per sample)
        auto osc=[&](int rate){return sine(position*std::max(.05,rate*.05));};
        auto eq=[&](int low,int high){
            c.bass+=std::min(.95,1000./hz)*(x-c.bass);
            c.treble+=std::min(.95,16500./hz)*(x-c.treble);
            x+=c.bass*(db(low)-1)+(x-c.treble)*(db(high)-1);
        };
        auto chorus=[&](double base,double rate,double depth){
            return tap(c,base+depth*(1+sine(position*rate+side*.25)));
        };
        auto feedback=[&](double amount){feed=x*std::clamp(amount*.01,-.965,.965);};
        switch(id){
        case TREMOLO: case AUTOPAN:{
            double t=cycle(position*std::max(.05,p[1]*.05)+(id==AUTOPAN?side*.5:0));
            double wave=p[0]==0?2*std::abs(2*t-1):p[0]==1?2*t:p[0]==3?2*(1-t):p[0]==4?double(t<.5)*2:1+sine(t);
            x*=blend(1,wave*.5,u(2));eq(p[3],p[4]);level=u(5);break;}
        case GRAPHEQ: case SPECTRUM:{
            constexpr double graphic[]={180,250,355,500,710,1000,1400,2000,2800,4000,5600,8000};
            constexpr double spectrum[]={250,500,1000,1250,2000,3150,4000,8000};
            bool g=id==GRAPHEQ;unsigned bands=g?12:8;double q=g?2.8:1+.4*p[8];
            for(unsigned i=0;i<bands;++i)x+=c.filter(i,dry,g?graphic[i]:spectrum[i],q,hz,1)*(db(p[i])-1)/q;
            level=g?db(p[12]):u(10);break;}
        case OCTAVE:
            if(c.last<=0 && x>0){c.subSign=-c.subSign;if(c.subSign>0)c.quarterSign=-c.quarterSign;}
            c.last=x;c.envelope+=.01*(std::abs(x)-c.envelope);
            x=x*u(2)+c.envelope*(c.subSign*u(1)+c.quarterSign*u(0));break;
        case RINGMOD:
            if(p[0]!=ringKey){ringKey=p[0];ringHz=20*std::exp2(8*u(0));}x*=sine(position*ringHz);wet=u(5);eq(p[3],p[4]);level=u(6);break;
        case HUMANIZR:{
            constexpr double formants[][2]={{800,1150},{400,1700},{300,2200},{500,900},{350,700}};
            auto a=formants[std::clamp(p[2],0,4)],b=formants[std::clamp(p[3],0,4)];
            double morph=std::clamp(p[8]*.01+.5*u(5)*osc(p[4]),0.,1.);
            if(p[0])x=std::tanh(x*(1+8*u(1)));
            x=.35*(c.filter(0,x,blend(a[0],b[0],morph),4,hz,1)+c.filter(1,x,blend(a[1],b[1],morph),5,hz,1));
            eq(p[9],p[10]);level=u(12);break;}
        case STEPFLT:case SUPFILT:{
            bool step=id==STEPFLT;
            unsigned seq=unsigned(cycle(position*std::max(.05,p[16]*.05)/16)*16);
            double cutoff=step?u(seq):u(2)+(p[5]?.35*u(8)*osc(p[7]):0);
            x=c.filter(0,x,35*std::exp2(9*std::clamp(cutoff,0.,1.)),.6+4*u(step?20:3),hz,p[step?18:0]);
            level=u(step?22:10)*db(p[step?21:4]);break;}
        case SLICER:{
            unsigned step=unsigned(cycle(position*(p[18]?bpm/15:std::max(.05,p[16]*.1))/16)*16);
            c.envelope+=(u(step)-c.envelope)/(1+hz*.00015*p[17]);x*=c.envelope;level=u(22);break;}
        case ROTARY:{
            double slow=c.filter(0,x,800,.707,hz,0),high=x-slow;
            double a=osc(p[p[4]?2:0]),b=osc(p[p[4]?3:1]);
            x=slow*u(8)*(1+(side?1:-1)*.45*b*u(9))+high*u(7)*(1+(side?1:-1)*.65*a*u(9));level=u(10);break;}
        case DUALPITCH:{
            double a=pitch(c,p[1]+.01*p[2],p[3]),b=pitch(c,p[5]+.01*p[6],p[7]);
            auto pan=[&](int v){return std::sqrt(std::clamp(.5+(side?1:-1)*v/128.,0.,1.));};
            x=blend(a*pan(p[4]),b*pan(p[8]),p[9]*.01);wet=p[10]*.01;level=u(11);break;}
        case FBKPITCH:x=pitch(c,p[1]+.01*p[2],p[3]);feedback(p[4]);eq(p[6],p[7]);wet=p[8]*.01;level=u(9);break;
        case MULTITAP:case QUADTAP:{
            bool multi=id==MULTITAP;x=0;
            for(int i=0;i<4;++i){double pan=multi?std::sqrt(std::clamp(.5+(side?1:-1)*p[4+i]/128.,0.,1.)):1;
                x+=.4*pan*u((multi?8:4)+i)*tap(c,p[i]);}
            feedback(p[multi?12:8]);wet=p[multi?16:10]*.01;level=u(multi?17:11);break;}
        case TIMECTRL:case TIMESKIP:case SHUFFLE:{
            double ms=delayMs;
            if(id==TIMESKIP && cycle(position*std::max(.1,p[1]*.05))>.5)ms*=.5;
            if(id==SHUFFLE && side)ms*=1+p[1]*.01;
            x=tap(c,ms);feedback(p[id==TIMECTRL?1:id==TIMESKIP?2:6]);
            unsigned end=programs[id].count;wet=p[end-2]*.01;level=u(end-1);break;}
        case REVDELAY:{
            double ms=std::max(2,p[1])*1.;double a=cycle(position*1000/ms),b=cycle(a+.5);
            x=blend(tap(c,2*b*ms),tap(c,2*a*ms),.5-.5*cosine(a));
            x=.5*(x*u(12)+tap(c,p[2])*u(13)+tap(c,p[3])*u(14));feedback(p[5]);wet=p[15]*.01;level=u(18);break;}
        case MODDELAY:x=chorus(p[side?2:1],p[5]*.05,u(6)*5);feedback(p[3]);wet=p[10]*.01;level=u(11);break;
        case HEXACHOR:
            x=0;for(int i=0;i<6;++i)x+=tap(c,p[0]+2+i*u(3)+5*u(2)*(1+sine(position*p[1]*.05+i/6.+side*u(5))))/6;
            wet=p[6]*.01;level=u(7);break;
        case TRMCHORS:x=chorus(p[0]+1,p[1]*.05,u(2)*5)*(.7+.3*sine(position*p[3]*.05+side*p[5]/360.));wet=p[6]*.01;level=u(7);break;
        case SLCRFLNG:{
            double clock=position*std::max(1,p[2])*4/60;
            x=(p[4] && cycle(clock)>.55?0:dry)*.65+.35*chorus(1,.35,3*u(5));break;}
        case STEPFLNG:{
            double time=std::floor(position*std::max(1,p[4]))/std::max(1,p[4]);
            x=tap(c,p[0]+.5+4*u(2)*(1+sine(time*p[1]*.05+side*p[5]/360.)));
            feedback(p[3]);wet=p[8]*.01;level=u(9);break;}
        case STEPPHSR:{
            double time=std::floor(position*std::max(1,p[7]))/std::max(1,p[7]);
            x=blend(x,phaser(c,x,time*p[2]*.05,4+2*std::clamp(p[0],0,3)),.5*u(8));level=u(11);break;}
        case LOFINOIS:{
            seed=1664525u*seed+1013904223u;double noise=(seed/4294967296.-.5);
            double scale=std::exp2(std::clamp(13-p[0],3,16));x=std::round(x*scale)/scale;
            x+=noise*(.07*u(5)+.04*u(8))+.05*u(11)*sine(position*(p[9]?60:50));wet=p[12]*.01;level=u(13);break;}
        case GTRAMP:
            if(p[0])x=std::tanh(x*(1+(.4+p[1])*(1+p[4])*u(2)))*u(3);
            eq((p[5]-64)/6,(p[7]-64)/6);
            if(p[10])x=c.filter(0,x,2200+140*p[11],.707,hz,0)*u(13);
            x+=dry*u(14);level=u(16);break;
        case BASSMLTI:
            if(p[0]){c.envelope=std::max(std::abs(x),c.envelope*.999);double threshold=db(p[1]);x*=std::min(1.,std::pow(threshold/std::max(.00001,c.envelope),.6))*db(p[5]);}
            if(p[6])x=std::tanh(x*(1+8*u(8)))*u(11);
            if(p[9])x=c.filter(0,x,1800+500*p[10],.707,hz,0);
            eq(p[13],p[18]);x=blend(x,chorus(p[23],p[20]*.05,4*u(21)),.3*u(27));break;
        case EPMULTI:
            if(p[0])x+=.3*u(3)*std::tanh(4*x*x*x);
            x=blend(x,phaser(c,x,position*p[6]*.05,4),.5*u(9));
            x=blend(x,chorus(p[14],p[11]*.05,4*u(12)),.35*u(18));
            if(p[19])x*=1-.5*u(23)*(1+osc(p[22]));
            break;
        case KBDMULTI:
            x=blend(x,x*osc(p[0]*40),p[1]*.01);eq(p[3],p[8]);
            x=blend(x,pitch(c,p[10]+.01*p[11],0),p[12]*.01);
            x=blend(x,phaser(c,x,position*p[14]*.05,4),.5*u(17));
            {double echo=tap(c,p[18]);feed=echo*p[19]*.01;x+=.5*echo*u(24);}break;
        }
        c.history[head]=float(std::clamp(dry+feed,-16.,16.));
        return blend(dry,x,wet)*level;
    }
public:
    // 2.4.37 (Omni): prepare() allocates (UI thread); load() / reset() / process() never allocate (audio thread)
    void prepare(unsigned rate){hz=std::clamp(rate,8000u,192000u);for(auto& c:channels)c.prepare(unsigned(hz));reset();}
    bool prepared() const {return !channels[0].history.empty();}
    void load(Effect effect){
        id=effect;p.fill(0);
        for(unsigned i=0;i<programs[id].count;++i)p[i]=programs[id].controls[i].initial;
        reset();
    }
    void reset(){
        for(auto& c:channels)c.clear();
        head=0;position=0;delayMs=p[0];seed=777;ringKey=-1;ringHz=0;
        shiftKey[0]=shiftKey[1]=1e9;shiftRatio[0]=shiftRatio[1]=0;
    }
    bool set(unsigned i,int value){
        if(i>=programs[id].count)return false;
        auto& control=programs[id].controls[i];p[i]=std::clamp(value,control.minimum,control.maximum);return true;
    }
    void process(float* left,float* right,unsigned frames,double bpm){
        if(!left||!right||left==right||!prepared())return;
        bpm=std::isfinite(bpm)?std::clamp(bpm,20.,400.):120.;
        // 2.4.37: per block (no division per sample): the delay glide (TimeCtrl / TimeSkip / Shuffle; TimeSkip's Accel is
        // control 3, the original read control 2) and the sample period
        const double glide=id==TIMECTRL||id==TIMESKIP||id==SHUFFLE?1/(1+hz*.0001*(1+p[id==TIMESKIP?3:2])):0;
        const double invHz=1/hz;
        for(unsigned i=0;i<frames;++i){
            if(glide>0)delayMs+=(p[0]-delayMs)*glide;   // 2.4.37: TimeSkip's Accel is control 3
            for(unsigned ch=0;ch<2;++ch){
                float& sample=(ch?right:left)[i];
                sample=float(frame(channels[ch],ch,std::isfinite(sample)?std::clamp(sample,-16.f,16.f):0.f,bpm));
            }
            position+=invHz;if(++head==channels[0].history.size())head=0;
        }
    }
};
}
}  // namespace omnisampler
