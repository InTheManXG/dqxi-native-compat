// 跳板在原映像前一页；调用必须保持这一页距离。
.section __TEXT,__dmmainx0
.p2align 14
b 1f
.space 0x4000-4
1: mov w0,#42
ret
.space 0x4000-8
.section __DMmainW,__img
.p2align 14
.space 0x8000
