# this sets up gdb to use openocd. You must start openocd first
target extended-remote :50000
mon reset init
set confirm off
starti

