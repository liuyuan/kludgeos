#!/bin/bash

qemu -hda obj/kern/bochs.img -hdb obj/fs/fs.img -parallel stdio \
     -smp 4 -no-kqemu -name kludgeOS \
     -net user -net nic,model=i82559er,macaddr=52:54:00:12:34:56 \
     -redir tcp:8080::80 -redir tcp:4242::10000 "$@" -std-vga
--test 
