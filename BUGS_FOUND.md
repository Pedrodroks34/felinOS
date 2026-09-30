# FelinOS / Gato - status de bugs

Revisado contra o código real (a lista anterior tinha vários itens já corrigidos ou incorretos).
Testado em QEMU (ISO via GRUB BIOS): `selftest` 35/35, `simplecc` compila e executa.

## Corrigidos
- **simplecc -> KERNEL PANIC (page fault em 0x19)**: `kernel_getchar()` chamava
  `vfs_read((struct vfs_file *)1, ...)` (ponteiro falso). Reescrito `kernel/sh/simplecc.c`:
  entrada via `input_getkey()`, gera código x86-64 nativo (antes gerava x86-32 com `int 0x80`,
  impossível de rodar no kernel), deslocamentos de 32 bits (antes 8 bits), tamanho do frame
  corrigido, sem vazamento de memória, com erros de sintaxe reportados, `for`, `%`, `&&`, `||`,
  `!`, chamadas com argumentos, recursão, e proteção contra loop infinito / recursão /
  divisão por zero (aborta o programa em vez de travar o kernel).
- **pmm.c `find_run()`**: uma corrida de frames podia "atravessar" o fim da memória e continuar
  no frame 0 (retornava região não contígua). Agora zera a corrida ao dar a volta.
- **gatofs.c `iread`/`iwrite`**: sem checagem de `ino`; um número de inode corrompido lia/escrevia
  bloco arbitrário. Agora retorna erro se `ino >= inode_count`.
- **gatofs.c `slot()`**: sem checagem do índice dentro da tabela; agora `i >= PPB` retorna 0.

- **net: `dhcp` (e qualquer UDP/TCP/ICMP com payload grande) dava KERNEL PANIC**: `udp_sendto`,
  `tcp` e `icmp` colocavam o payload com `netbuf_push()`, que consome os 128 bytes reservados
  para cabeçalhos. Payload > ~86 bytes (DHCP tem ~300) fazia `netbuf_push` retornar NULL e o
  `memcpy` escrevia em 0x0 -> page fault. Novo `netbuf_put()` (append com checagem de limite) e
  retorno de erro em vez de crash. Isso também deixava o shell sem prompt quando `dhcp` estava no
  `~/.vshrc`, pois o `.vshrc` roda antes do prompt.
- **shell**: `ifconfig dns <ip>` e `ifconfig <ip> <mask> <gw> <dns>` para configurar o DNS à mão.

## Falsos positivos da lista antiga (verificados, código já está correto)
- `refs_frames` do PMM (já é `/256`), `find_run` sem `search_from` (usa), `static tmp[BS]` em
  `pread_i/pwrite_i/dir_*` (não existem no código), `serial_read_nonblock` sem `irq_save`
  (tem), `sched_wake_one` acordar tarefa já na fila (só acorda `TASK_BLOCKED`),
  `sched_irq_return` sem checar `preempt` (checa `c->preempt == 0`).

## Observações (não alteradas)
- Makefile fixa os VMAs do `objcopy` (`.rodata=0x143000` etc.). O boot funciona porque o
  carregador usa o endereço físico (LMA), mas os valores não batem com o ELF atual
  (`.rodata` real em 0x13c000). Convém gerá-los a partir do ELF (`objdump -h`).
- `make run-kernel` (-kernel) não bootou no QEMU 8.2 deste ambiente; use a ISO (`make run`).
