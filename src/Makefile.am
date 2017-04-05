# BlueALSA - Makefile.am
# Copyright (c) 2016-2017 Arkadiusz Bokowy

EXTRA_DIST = bluealsa.service
bluealsa_data_DATA = bluealsa.service
bluealsa_datadir = /lib/systemd/system

bin_PROGRAMS = bluealsa
SUBDIRS = asound

bluealsa_SOURCES = \
	shared/log.c \
	shared/rt.c \
	bluealsa.c \
	bluez.c \
	bluez-iface.c \
	ctl.c \
	io.c \
	transport.c \
	utils.c \
	main.c

AM_CFLAGS = \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@AAC_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@BLUEZ_LIBS@ \
	@GLIB2_LIBS@ \
	@GIO2_LIBS@ \
	@AAC_LIBS@ \
	@SBC_LIBS@
