#pragma once
#include <cstdint>
#include <cstring>
typedef int8_t i8;
typedef uint8_t u8;
typedef int16_t i16;
typedef uint16_t u16;
struct CPU6502{
u8 A,X,Y,P,SP; u16 PC; u8*ram;
void reset(u8*m){ram=m;A=X=Y=0;P=0x24;SP=0xFD;PC=r16(0xFFFC);}
void push(u8 v){ram[0x100|SP--]=v;}
u8 pop(){return ram[0x100|++SP];}
u8 r8(u16 a){return ram[a];}
u16 r16(u16 a){return r8(a)|(u16)r8(a+1)<<8;}
u16 zp16(u8 p){return r8(p)|(u16)r8(p+1&0xFF)<<8;}
u8 f8(){return ram[PC++];}
u16 f16(){return f8()|(u16)f8()<<8;}
void snz(u8 v){P=(P&0x7D)|(v?v&0x80:2);}
void adc(u8 v){
if(P&8){
u8 Ao=A,ci=P&1;i16 al=(A&0xF)+(v&0xF)+ci;if(al>9)al+=6;
i16 ah=(A>>4)+(v>>4)+(al>0xF); u8 mid=(u8)((ah<<4)|(al&0xF));
P=(P&0x3C)|((~(Ao^v)&(Ao^mid)&0x80)?0x40:0)|(ah>9?1:0)|(mid&0x80)|((u8)(Ao+v+ci)?0:2);
if(ah>9)ah+=6;A=((ah&0xF)<<4)|(al&0xF);return;}
u16 t=A+v+(P&1);P=(P&0x3C)|((~(A^v)&(A^t)&0x80)?0x40:0)|(t>0xFF?1:0);snz(A=t);}
void sbc(u8 v){
if(P&8){
u8 Ao=A; i16 lo=(A&0xF)+(P&1)-(v&0xF)-1;bool blo=lo<0;if(blo)lo=((lo-6)&0xF);
i16 hi=(A>>4)-(v>>4)-blo;if(hi<0)hi=((hi-6)&0xF);
A=((hi&0xF)<<4)|(lo&0xF);
u16 t2=Ao+(P&1)-v-1;P=(P&0x3C)|((((Ao^v)&0x80)&&((Ao^(u8)t2)&0x80))?0x40:0)|(t2<0x100?1:0)|((u8)t2&0x80)|((u8)t2?0:2);return;}
u16 t=A+(P&1)-v-1;P=(P&0x3C)|((((A^v)&0x80)&&((A^t)&0x80))?0x40:0)|(t<0x100?1:0);snz(A=t);}
void cmp(u8 r, u8 v){P=(P&0x7C)|(r>=v?1:0);snz(r-v);}
void intr(u16 v){push(PC>>8);push(PC);push(P|0x20);P|=4;PC=r16(v);}
void irq(){if(!(P&4))intr(0xFFFE);}
void nmi(){intr(0xFFFA);}
u16 ea;bool imm;
u8 mode(u8 m){imm=false;switch(m){
case 0:ea=zp16((f8()+X)&0xFF);return r8(ea);case 1:ea=f8();return r8(ea);
case 2:imm=true;return f8();case 3:ea=f16();return r8(ea);
case 4:ea=zp16(f8())+Y;return r8(ea);case 5:ea=(u8)(f8()+X);return r8(ea);
case 6:ea=f16()+Y;return r8(ea);case 7:ea=f16()+X;return r8(ea);}return 0;}
u16 rmea(u8 op){return op&8?f16()+(op&0x10?X:0):(u8)(f8()+(op&0x10?X:0));}
void bit(u16 a){ u8 b=r8(a);P=(P&0x3D)|(b&0xC0)|((A&b)?0:2);}
void step(){
static constexpr u8 bfl[]={0x80,0x40,0x01,0x02};
u8 op=f8(),v,t;
switch(op){
case 0x00:PC++;push(PC>>8);push(PC);push(P|0x30);P|=4;PC=r16(0xFFFE);return;
case 0x20:{ u8 lo=f8();push(PC>>8);push(PC);PC=lo|(u16)f8()<<8;return;}
case 0x40:P=pop()|0x20;P&=~0x10;PC=pop();PC|=(u16)pop()<<8;return;
case 0x60:PC=pop();PC|=(u16)pop()<<8;PC++;return;
case 0x4C:PC=f16();return;
case 0x6C:{ u16 a=f16();PC=r8(a)|((a&0xFF)==0xFF?(u16)r8(a&0xFF00)<<8:(u16)r8(a+1)<<8);return;}
case 0x10:case 0x30:case 0x50:case 0x70:case 0x90:case 0xB0:case 0xD0:case 0xF0:
v=f8();if(!!(P&bfl[(op>>6)&3])==!!(op&0x20))PC+=(i8)v;return;
case 0x18:P&=~1;return;case 0x38:P|=1;return;case 0x58:P&=~4;return;case 0x78:P|=4;return;
case 0xB8:P&=~0x40;return;case 0xD8:P&=~8;return;case 0xF8:P|=8;return;
case 0x08:push(P|0x30);return;case 0x28:P=pop()|0x20;P&=~0x10;return;
case 0x48:push(A);return;case 0x68:snz(A=pop());return;
case 0x8A:snz(A=X);return;case 0x98:snz(A=Y);return;case 0x9A:SP=X;return;
case 0xA8:snz(Y=A);return;case 0xAA:snz(X=A);return;case 0xBA:snz(X=SP);return;
case 0xE8:snz(++X);return;case 0xC8:snz(++Y);return;case 0xCA:snz(--X);return;case 0x88:snz(--Y);return;
case 0x24:bit(f8());return;case 0x2C:bit(f16());return;
case 0xA9:snz(A=f8());return;case 0xA2:snz(X=f8());return;case 0xA0:snz(Y=f8());return;
case 0xE0:cmp(X,f8());return;case 0xC0:cmp(Y,f8());return;case 0xC9:cmp(A,f8());return;
case 0x0A:case 0x2A:case 0x4A:case 0x6A:
if(op&0x40){v=A&1;A>>=1;if(op&0x20&&P&1)A|=0x80;}else{v=A>>7;A<<=1;if(op&0x20&&P&1)A|=1;}
P=(P&~1)|v;snz(A);return;
case 0xEA:return;}
if((op&3)==1){
v=mode((op>>2)&7);switch(op>>5){
case 0:snz(A|=v);return;case 1:snz(A&=v);return;case 2:snz(A^=v);return;
case 3:adc(v);return;case 4:if(!imm)ram[ea]=A;return;
case 5:snz(A=v);return;case 6:cmp(A,v);return;case 7:sbc(v);return;}return;}
switch(op){
case 0x06:case 0x0E:case 0x16:case 0x1E:ea=rmea(op);v=r8(ea);P=(P&~1)|(v>>7);snz(ram[ea]=v<<1);return;
case 0x26:case 0x2E:case 0x36:case 0x3E:ea=rmea(op);v=r8(ea);t=P&1;P=(P&~1)|(v>>7);snz(ram[ea]=(v<<1)|t);return;
case 0x46:case 0x4E:case 0x56:case 0x5E:ea=rmea(op);v=r8(ea);P=(P&~1)|(v&1);snz(ram[ea]=v>>1);return;
case 0x66:case 0x6E:case 0x76:case 0x7E:ea=rmea(op);v=r8(ea);t=P&1;P=(P&~1)|(v&1);snz(ram[ea]=(v>>1)|(t?0x80:0));return;
case 0xE6:case 0xEE:case 0xF6:case 0xFE:ea=rmea(op);v=r8(ea);snz(ram[ea]=++v);return;
case 0xC6:case 0xCE:case 0xD6:case 0xDE:ea=rmea(op);v=r8(ea);snz(ram[ea]=--v);return;}
switch(op){
case 0xA6:snz(X=r8(f8()));return;case 0xAE:snz(X=r8(f16()));return;
case 0xB6:snz(X=r8((f8()+Y)&0xFF));return;case 0xBE:snz(X=r8(f16()+Y));return;
case 0xA4:snz(Y=r8(f8()));return;case 0xAC:snz(Y=r8(f16()));return;
case 0xB4:snz(Y=r8((f8()+X)&0xFF));return;case 0xBC:snz(Y=r8(f16()+X));return;
case 0x86:ram[f8()]=X;return;case 0x8E:ram[f16()]=X;return;case 0x96:ram[(f8()+Y)&0xFF]=X;return;
case 0x84:ram[f8()]=Y;return;case 0x8C:ram[f16()]=Y;return;case 0x94:ram[(f8()+X)&0xFF]=Y;return;
case 0xE4:cmp(X,r8(f8()));return;case 0xEC:cmp(X,r8(f16()));return;
case 0xC4:cmp(Y,r8(f8()));return;case 0xCC:cmp(Y,r8(f16()));return;}
}
};
