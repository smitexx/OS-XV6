#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
extern int mappages(pde_t * pgdir, void * va, uint size, uint pa, int perm);
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  //Int disco duro
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;
  case T_PGFLT:
    //añadir que la direccion sea del kernel y no del usuario comprobar que la direccion esta por encima del kernel
    if( (myproc() == 0 || (tf->cs&3) == 0) && rcr2() > KERNBASE){
      // In kernel, it must be our mistake.
      cprintf("unexpected page fault from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("page fault into kernel");
    }
    //Aqui hay que comprobar si la direccion esta dentro del espacio de direcciones del proceso;
    //si cr2  > proc->size entonces fuera de espacio de direcciones 
    //Tambien hay que comprobar si tienes permiso sobre esa página nueva de memoria.
    if (rcr2() > myproc()->sz){
      cprintf("address out of proccess memory range\n");
      myproc()->killed = 1;
      break;
    }
    //si cr2 dentro de espacio de direcciones puede ser pagina de guarda o memoria dinámica entonces para saber
    //cual fue miramos el codigo de error tf->err & 1 tiene que ser 1 y entonces matamos el proceso (test 2)
    if ((tf->err & 1) == 1){
      cprintf("access to guard page not allowed\n");
      myproc()->killed = 1;
      break;
    }
    char * mem;
    mem = kalloc();
    if(mem == 0){
      cprintf("page fault out of memory\n");
      myproc()->killed = 1;
      break;
    }
    //Porque las paginas libres se pone todo a 1
    memset(mem, 0, PGSIZE);
    //En el registro de control numero 2 la direccion a la que se estaba accediendo
    if(mappages(myproc()->pgdir, (char*)PGROUNDDOWN(rcr2()), PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("page fault out of memory\n");
      myproc()->killed = 1;
      kfree(mem);
    }
    break;
  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
