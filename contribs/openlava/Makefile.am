#
# Makefile for openlava scripts

AUTOMAKE_OPTIONS = foreign

bin_SCRIPTS = bjobs bkill bsub lsid

bjobs:
bkill:
bsub:
lsid:

_perldir=$(exec_prefix)`perl -e 'use Config; $$T=$$Config{installsitearch}; $$P=$$Config{installprefix}; $$P1="$$P/local"; $$T =~ s/$$P1//; $$T =~ s/$$P//; print $$T;'`

install-binSCRIPTS: $(bin_SCRIPTS)
	@$(NORMAL_INSTALL)
	test -z "$(DESTDIR)$(bindir)" || $(mkdir_p) "$(DESTDIR)$(bindir)"
	@list='$(bin_SCRIPTS)'; for p in $$list; do \
	   echo "sed 's%use lib .*%use lib qw(${_perldir});%' $(top_srcdir)/contribs/openlava/$$p.pl > $(DESTDIR)$(bindir)/$$p"; \
	   sed "s%use lib .*%use lib qw(${_perldir});%" $(top_srcdir)/contribs/openlava/$$p.pl >$(DESTDIR)$(bindir)/$$p; \
	   chmod 755 $(DESTDIR)$(bindir)/$$p;\
	done

uninstall-binSCRIPTS:
	@$(NORMAL_UNINSTALL)
	@list='$(bin_SCRIPTS)'; for p in $$list; do \
	  echo " rm -f '$(DESTDIR)$(bindir)/$$p'"; \
	  rm -f "$(DESTDIR)$(bindir)/$$p"; \
	done

clean:
