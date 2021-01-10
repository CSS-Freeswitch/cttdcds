include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_smsctl

mod_LTLIBRARIES = mod_smsctl.la
mod_smsctl_la_SOURCES  = mod_smsctl.c
mod_smsctl_la_CFLAGS   = $(AM_CFLAGS)
mod_smsctl_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_smsctl_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
