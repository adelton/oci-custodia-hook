libexec_PROGRAMS = oci-custodia-hook
oci_custodia_hook_SOURCES= src/custodiahook.c

oci_custodia_hook_CFLAGS = -Wall -Wextra -std=c99 $(YAJL_CFLAGS)
oci_custodia_hook_LDADD = $(YAJL_LIBS)
oci_custodia_hook_CFLAGS += $(SELINUX_CFLAGS)
oci_custodia_hook_LDADD += $(SELINUX_LIBS)
oci_custodia_hook_CFLAGS += $(LIBMOUNT_CFLAGS)
oci_custodia_hook_LDADD += $(LIBMOUNT_LIBS)

dist_man_MANS = oci-custodia-hook.1
EXTRA_DIST = README.md LICENSE

oci-custodia-hook.1: doc/oci-custodia-hook.1.md
	go-md2man -in doc/oci-custodia-hook.1.md -out oci-custodia-hook.1

dist: oci-custodia-hook.spec 
	spectool -g oci-custodia-hook.spec

rpm: dist
	rpmbuild --define "_sourcedir `pwd`" --define "_specdir `pwd`" \
	--define "_rpmdir `pwd`" --define "_srcrpmdir `pwd`" -ba oci-custodia-hook.spec 

clean-local:
	-rm -f oci-custodia-hook.1 *~
	-rm -f oci-custodia-hook-*.tar.gz
	-rm -f oci-custodia-hook-*.rpm

