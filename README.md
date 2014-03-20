# BUSE - A block device in userspace

This piece of software was branched from https://github.com/acozzette/BUSE,
which allows the development of Linux file systems that run in userspace. 
The goal of BUSE is to allow virtual block devices to run in userspace as well. 
Currently BUSE is used to provide block device support for SDFS.

Implementing a block device with BUSE is fairly straightforward. Simply fill
`struct buse_operations` (declared in `buse.h`) with function pointers that
define the behavior of the block device, and set the size field to be the
desired size of the device in bytes. Then call `buse_main` and pass it a
pointer to this struct. `busexmp.c` is a simple example example that shows how
this is done.

The implementation of BUSE itself relies on NBD, the Linux network block device,
which allows a remote machine to serve requests for reads and writes to a
virtual block device on the local machine. BUSE sets up an NBD server and client
on the same machine, with the server executing the code defined by the BUSE
user.

Different than acozzette's release, this version is multithreaded.
