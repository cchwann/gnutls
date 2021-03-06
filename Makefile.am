## Process this file with automake to produce Makefile.in
# Copyright (C) 2000-2012 Free Software Foundation, Inc.
#
# Author: Nikos Mavrogiannopoulos
#
# This file is part of GnuTLS.
#
# This file is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This file is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this file; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc --disable-valgrind-tests

SUBDIRS = gl lib extra

if ENABLE_DANE
SUBDIRS += libdane
endif

SUBDIRS += po
if ENABLE_TOOLS
SUBDIRS += src/gl src
else
SUBDIRS += src/gl
endif

if ENABLE_TESTS
SUBDIRS += tests
endif

if HAVE_GUILE
SUBDIRS += guile
endif

if ENABLE_MANPAGES
SUBDIRS += doc/manpages
endif

if ENABLE_DOC
SUBDIRS += doc
endif

ACLOCAL_AMFLAGS = -I m4 -I gl/m4 -I src/libopts/m4 -I src/gl/m4

EXTRA_DIST = cfg.mk maint.mk .clcopying

README-alpha: README-alpha.md
	cp $^ $@
	chmod 444 $@

README: README-install.md
	cp $^ $@
	chmod 444 $@

TMPFILE="abi-temp.xml"

abi-check:
	@rm -f $(TMPFILE)
	@echo "Checking libgnutls ABI"
	@echo "<version>$(VERSION)</version>" >$(TMPFILE)
	@echo "<headers>$(srcdir)/lib/includes/gnutls" >>$(TMPFILE)
	@echo "$(builddir)/lib/includes/gnutls</headers>" >>$(TMPFILE)
	@echo "<libs>$(builddir)/lib/.libs</libs>" >>$(TMPFILE)
	test ! -f "$(srcdir)/devel/ABI-$$(uname -m).dump" || \
		abi-compliance-checker -abi -lib gnutls -old "$(srcdir)/devel/ABI-$$(uname -m).dump" -new $(TMPFILE) -skip-symbols $(srcdir)/devel/abi-unchecked-symbols 
	@echo "Checking libgnutls-dane ABI"
	@echo "<version>$(VERSION)</version>" >$(TMPFILE)
	@echo "<headers>$(srcdir)/libdane/includes/gnutls</headers>" >>$(TMPFILE)
	@echo "<libs>$(builddir)/libdane/.libs</libs>" >>$(TMPFILE)
	test ! -f "$(srcdir)/devel/ABI-dane-$$(uname -m).dump" || abi-compliance-checker -abi -lib gnutls-dane -old "$(srcdir)/devel/ABI-dane-$$(uname -m).dump" -new $(TMPFILE)
	@rm -f $(TMPFILE)

@CODE_COVERAGE_RULES@

dist-hook: abi-check
	make -C doc/ compare-makefile
	make -C doc/ compare-exported
	make -C doc/manpages compare-makefile
	@echo "*****************************************************************"
	@echo "Checking symbol changes. If the next step fails, verify that the"
	@echo "symbols added or removed are correct and copy symbols.last.tmp as"
	@echo "symbols.last"
	@echo "*****************************************************************"
	objdump -T $(srcdir)/lib/.libs/libgnutls.so.30  | grep -v ' \*UND\*' | awk '{print $$7 "@" $$6;}' | grep -v GNUTLS_FIPS140 | grep -v GNUTLS_PRIVATE |  sort -u >symbols.last.tmp
	diff -u symbols.last symbols.last.tmp >/dev/null 2>&1
	rm -f symbols.last.tmp
	rm -f ChangeLog
	make ChangeLog
	cp -f ChangeLog $(distdir)/
	$(SED) 's/\@VERSION\@/$(VERSION)/g' -i $(distdir)/src/args-std.def
	$(SED) 's/\@YEAR\@/$(YEAR)/g' -i $(distdir)/src/args-std.def
	$(SED) 's/\@PACKAGE_BUGREPORT\@/$(PACKAGE_BUGREPORT)/g' -i $(distdir)/src/args-std.def
	cd $(distdir)/src/ && for i in *.def;do \
		if test x"$$i" = x"args-std.def";then  continue; fi; \
		autogen $$i; \
		cp $$i $$i.tmp; \
		$(SED) -i 's/@subheading \(.*\)/@*\n@var{\1}\n@*/' $$i.tmp; \
		autogen -Tagman-cmd.tpl $$i.tmp; \
		rm -f $$i.tmp; \
		mv -f *.1 ../doc/manpages/; \
	done
	cd $(distdir)/src/ && for i in *-args.c *-args.h;do \
		mv $$i $$i.bak; \
	done
	@echo "*****************************************************************"
	@echo "Checking whether included libopts matches the system's. If the"
	@echo "check fails upgrade the included libopts."
	@echo "*****************************************************************"
	test "`autoopts-config libsrc|cut -d '-' -f 2|sed 's/.tar.gz//'`" = "`cat src/libopts/autoopts/options.h |grep OPTIONS_VERSION_STRING|cut -d '"' -f 2|sed 's/:/./g'`"
	touch $(distdir)/doc/*.html $(distdir)/doc/*.pdf $(distdir)/doc/*.info

.PHONY: abi-check
