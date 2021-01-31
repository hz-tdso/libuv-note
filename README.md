# libuv-note

参考了如下资料，对libuv-1.x的源码进行注释：
https://blog.butonly.com/categories/%E6%BA%90%E7%A0%81%E5%88%86%E6%9E%90/


# 理解指针：
```c
#include <stdio.h>


struct as{

    int* a[20];

} ast;


void f(struct as *loop){

    printf("a, %p", &loop->a);

}
```

通过gcc -S -o testp.s testp.c得到的汇编代码：
```asm
  .file "testp.c"

  .text

  .comm ast,160,32

  .section  .rodata

.LC0:

  .string "a, %p"

  .text

  .globl  f

  .type f, @function

f:

.LFB0:

  .cfi_startproc

  endbr64

  pushq %rbp

  .cfi_def_cfa_offset 16

  .cfi_offset 6, -16

  movq  %rsp, %rbp

  .cfi_def_cfa_register 6

  subq  $16, %rsp

  movq  %rdi, -8(%rbp)

  movq  -8(%rbp), %rax

  movq  %rax, %rsi

  leaq  .LC0(%rip), %rdi

  movl  $0, %eax

  call  printf@PLT

  nop

  leave

  .cfi_def_cfa 7, 8

  ret

  .cfi_endproc

.LFE0:

  .size f, .-f

  .globl  main

  .type main, @function
```

c代码第8行改为如下：
```c
printf("a, %p", &loop->a[0]);
```
对应的汇编：

```
  .file "testp.c"

  .text

  .comm ast,160,32

  .section  .rodata

.LC0:

  .string "a, %p"

  .text

  .globl  f

  .type f, @function

f:

.LFB0:

  .cfi_startproc

  endbr64

  pushq %rbp

  .cfi_def_cfa_offset 16

  .cfi_offset 6, -16

  movq  %rsp, %rbp

  .cfi_def_cfa_register 6

  subq  $16, %rsp

  movq  %rdi, -8(%rbp)

  movq  -8(%rbp), %rax

  movq  %rax, %rsi

  leaq  .LC0(%rip), %rdi

  movl  $0, %eax

  call  printf@PLT

  nop

  leave

  .cfi_def_cfa 7, 8

  ret

  .cfi_endproc

.LFE0:

  .size f, .-f

  .globl  main

  .type main, @function
```

c代码第8行改为如下：
```c
printf("a, %p", &loop->a[1]);
```

得到：
```
  .file "testp.c"

  .text

  .comm ast,160,32

  .section  .rodata

.LC0:

  .string "a, %p"

  .text

  .globl  f

  .type f, @function

f:

.LFB0:

  .cfi_startproc

  endbr64

  pushq %rbp

  .cfi_def_cfa_offset 16

  .cfi_offset 6, -16

  movq  %rsp, %rbp

  .cfi_def_cfa_register 6

  subq  $16, %rsp

  movq  %rdi, -8(%rbp)

  movq  -8(%rbp), %rax

  addq  $8, %rax

  movq  %rax, %rsi

  leaq  .LC0(%rip), %rdi

  movl  $0, %eax

  call  printf@PLT

  nop

  leave

  .cfi_def_cfa 7, 8

  ret

  .cfi_endproc

.LFE0:

  .size f, .-f

  .globl  main

  .type main, @function
```

从上面的汇编代码的变化可以得出结论：
当p->a与p->a[0]是等价的，&p->a与&p->a[0]也是等价的。


# 理解IO观察者
uv_poll_t封装了io观察者，在初始化uv_poll_t的时候，会将内置的uv__poll_io函数挂到io观察者上。
在启动uv_poll_t的时候：
1. 会将应用程序提供的回调挂到uv_poll_t上。
2. 将io观察者插入到loop的watcher_queue中，并将地址写入loop的watchers数组。

事件循环的轮询阶段会调用uv__io_poll函数。此函数会把loop的watcher_queue上的所有的io观察者注册到epoll实例中，然后通过linux的系统调用epoll_pwait阻塞等待epoll_event事件。当事件到达时，通过loop的watchers数组快速找到对应的io观察者，调用其上的回调函数uv__poll_io，uv__poll_io将会调用应用程序提供的回调函数。

事件循环的pending callbacks阶段会调用uv__run_pending函数。此函数会依次调用loop的pending_queue上的所有的io观察者上的uv__poll_io回调。


# 理解线程池与事件循环线程之间的通信机制
当loop的async_io_watcher中的io观察者上有事件发生时，事件循环线程会被唤醒，然后事件循环线程调用loop的async_io_watcher中的io观察者上的回调uv__async_io。此函数会去遍历loop的async_handles队列，如果队列中的handle的pending状态不为0，就调用handle上的回调（由应用程序提供）。
loop的async_handles队列中的handle除了一个之外均由应用程序提供，那一个特殊的handle是loop内嵌的wq_async(也是uv_async_t类型)。

应用程序提交的任务的类型是uv_work_t，由应用程序提供任务需要的两个回调函数uv_work_cb和uv_after_work_cb。uv_work_t包装了uv__work（libuv内部的任务表示方法）。uv__work有两个回调函数指针work和done，分别指向libuv内部的函数uv__queue_work和uv__queue_done。uv__queue_work和uv__queue_done分别负责调用uv_work_t上的work_cb和after_work_cb。可以将这种处理方式理解成代理模式。

任务会被入到线程池的任务队列中，然后被分配给线程池中的线程执行，实际上线程执行的是任务的work函数（由应用程序提供）。当线程执行完毕之后，会将任务插入到loop的wq队列中，然后设置loop的wq_async的pending状态，再向loop的async_io_watcher中的io观察者所观察的fd上写数据，从而在下一次epoll_pwait的时候能够唤醒事件循环线程。
被唤醒的事件循环线程会执行wq_async上的回调uv__work_done，此函数会遍历loop的wq队列，调用每个任务的done函数（应用程序提供）。