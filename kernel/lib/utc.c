#include <m5os/wall_time.h>
#include <string.h>
static unsigned days(unsigned y,unsigned m) {
    static const unsigned d[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return d[m-1]+(m==2 && y%4==0 && (y%100!=0 || y%400==0));
}
bool utc_to_epoch(const utc_time_t *t,uint64_t *out) {
    if(!t || !out || t->year<2000 || t->year>2099 || !t->month || t->month>12 ||
        !t->day || t->day>days(t->year,t->month) || t->hour>23 || t->minute>59 || t->second>59) return false;
    uint64_t d=10957; /* days from Unix epoch to 2000-01-01 */
    for(unsigned y=2000;y<t->year;y++) d+=365+(y%4==0);
    for(unsigned m=1;m<t->month;m++) d+=days(t->year,m);
    *out=(d+t->day-1)*86400+t->hour*3600+t->minute*60+t->second;return true;
}
bool utc_from_epoch(uint64_t epoch,utc_time_t *t) {
    if(!t || epoch<946684800u || epoch>4102444799ULL) return false;
    uint64_t d=epoch/86400-10957;unsigned rem=epoch%86400,y=2000,m=1;
    while(d>=365u+(y%4==0)) { d-=365u+(y%4==0);y++; }
    while(d>=days(y,m)) { d-=days(y,m);m++; }
    *t=(utc_time_t){y,m,(unsigned)d+1,rem/3600,(rem%3600)/60,rem%60};return true;
}
bool utc_parse(const char *s,utc_time_t *t) {
    if(!s || !t || strlen(s)!=20 || s[4]!='-' || s[7]!='-' || s[10]!='T' || s[13]!=':' || s[16]!=':' || s[19]!='Z')return false;
    unsigned values[6]={0},offsets[]={0,5,8,11,14,17},lengths[]={4,2,2,2,2,2};
    for(unsigned i=0;i<6;i++)for(unsigned j=0;j<lengths[i];j++) {
        char c=s[offsets[i]+j];if(c<'0'||c>'9')return false;values[i]=values[i]*10+(unsigned)(c-'0');
    }
    utc_time_t v={values[0],values[1],values[2],values[3],values[4],values[5]};
    uint64_t epoch;if(!utc_to_epoch(&v,&epoch))return false;*t=v;return true;
}
