#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "libatrac9.h"
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); fseek(f,0,2); long n=ftell(f); fseek(f,0,0); unsigned char*d=malloc(n); fread(d,1,n,f);
  unsigned char*data=0; long dlen=0; for(long i=12;i<n-8;){ unsigned sz=*(unsigned*)(d+i+4); if(!memcmp(d+i,"data",4)){data=d+i+8;dlen=sz;break;} i+=8+sz+(sz&1);}
  int fb=1536; unsigned char cfg[4]={0xFE,(7<<4),0,0}; int v=((fb-1)<<5)|(2<<3); cfg[2]=v>>8; cfg[3]=v&0xff;
  void*h[16]; for(int c=0;c<16;c++){h[c]=Atrac9GetHandle(); Atrac9InitDecoder(h[c],cfg);}
  float pcm[256]; long sfn=dlen/6144; int errs=0; FILE*o=fopen("w.raw","wb");
  for(long s=0;s<sfn;s++){ long pos=s*6144, end=(s+1)*6144;
    for(int fr=0;fr<4;fr++) for(int c=0;c<16;c++){
      while(pos+8<=end && !memcmp(data+pos,"\1\1\1\1\1\1\1\1",8)){ while(pos<end&&data[pos]==1) pos++; }
      int used=0; int e=Atrac9DecodeF32(h[c],data+pos,pcm,&used,0);
      if(getenv("V")&&s<3) printf("sf %ld fr %d ch %d pos %ld used %d e %x\n",s,fr,c,pos-s*6144,used,e);
      if(e){ if(errs<5) printf("sf %ld fr %d ch %d pos %ld err %x\n",s,fr,c,pos-s*6144,e); errs++; used=0; memset(pcm,0,sizeof pcm); }
      pos+=used; if(c==0) fwrite(pcm,4,256,o);
    }
  }
  printf("superframes %ld errs %d\n",sfn,errs); fclose(o);
}
