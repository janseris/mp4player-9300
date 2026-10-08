import sys, math
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
w,h=int(sys.argv[3]),int(sys.argv[4])
fs=w*h + 2*((w//2)*(h//2))
na,nb=len(a)//fs,len(b)//fs
n=min(na,nb); exact=0; worst=99.0; first_bad=-1
for i in range(n):
    fa=a[i*fs:(i+1)*fs]; fb=b[i*fs:(i+1)*fs]
    if fa==fb: exact+=1; continue
    if first_bad<0: first_bad=i
    se=sum((x-y)*(x-y) for x,y in zip(fa[:w*h],fb[:w*h]))
    mse=se/(w*h); p=99 if mse==0 else 10*math.log10(255*255/mse)
    worst=min(worst,p)
print("frames mine=%d ref=%d exact=%d/%d first_mismatch=%d worstY_PSNR=%.2f"%(na,nb,exact,n,first_bad,worst))
