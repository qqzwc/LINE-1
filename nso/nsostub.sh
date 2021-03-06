#!/bin/bash
# Shell script to build the source file for a "nativeso" stub library.
# $Id: nsostub.sh,v 1.2 2001/03/23 14:57:33 mvines Exp $
#
# Copyright (C) 2001  Michael Vines
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
#

if [ -z $1 ]; then
  echo usage: nsostub.sh libwhatever.so
  exit;
fi

cat <<-EOF
/* autogenerated from $1 -- `date` */

#include "nativeso.h"
#include "internal_syscalls.h"

static char *soname = "$1";

#define LINE_SYM(name, index) \\
  asm("\n.text"); \\
  asm("\n.align 4"); \\
  asm("\n.globl " #name); \\
  asm(".type " #name ", @function"); \\
  asm("\n" #name ":\n"); \\
  asm("  pushl %eax"); \\
  asm("  movl symlist + " #index ", %eax" ); \\
  asm("  cmpl $" "0, %eax" ); \\
  asm("  jnz  jmpto_" #name); \\
  asm("  call LINE_fixup"); \\
  asm("\njmpto_" #name ":"); \\
  asm("  popl %eax"); \\
  asm("  jmpl *symlist+" #index ); 


static struct nativeso_symtable symlist[] = {
EOF

SYMS=`nm $1 | grep " T " | sed -e 's/^.* T //'`

for sym in $SYMS; do
  echo "  {0, \"$sym\"},"
done  

cat <<-EOF
  {0, 0}
};


asm("\n.text"); 
asm("\n.align 4"); 
asm("\nLINE_fixup:"); 
asm("pushl %ebx");
asm("pushl %ecx");
asm("movl $" #SYSCALL_NSO_FIXUP ", %eax");
asm("leal symlist, %ebx");
asm("movl soname, %ecx");
asm("int $" "0x80"); 
asm("popl %ecx");
asm("popl %ebx");
asm("ret"); 

EOF

I=0
for sym in $SYMS; do
  echo "LINE_SYM($sym, $[$I * 8])"
  I=$[$I+1]
done

