# this sets up gdb to use openocd. You must start openocd first
target extended-remote :50000
mon reset init
set confirm off
load 
break main
break hal_lld_peripheral_unreset
break chRegSetThreadName
break _unhandled_exception
break __port_switch
bt
