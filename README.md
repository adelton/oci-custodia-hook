# oci-custodia-hook
===================

When built and placed to `/usr/libexec/oci/hooks.d`, the binary will be
called and will bind mount `/var/run/custodia/custodia.sock` to every
container which gets run, unless that bind mount already exists.
