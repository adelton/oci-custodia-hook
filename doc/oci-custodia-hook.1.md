oci-custodia-hook(1) -- OCI custodia hook
=========================================

## SYNOPSIS

`oci-custodia-hook` prestart [container.json]

`oci-custodia-hook` poststop

## DESCRIPTION

Bind mounts `/var/run/custodia/custodia.sock` to every container
which gets run.

It is expected to receive the state of the container including the pid and ID over stdin
as defined in the OCI specification (https://github.com/opencontainers/specs).

## ARGUMENTS

  * `prestart`:  Setup the systemd environment in a container

  * `container.json`: file which describes the container; in the fugure we might add filtering based on the container configuration.
  
  * `poststop`:  Ignored


## AUTHORS
Jan Pazdziora <jpazdziora@gmail.com>

Original code of oci-systemd-hook
Mrunal Patel <mrunalp@gmail.com>
Dan Walsh <dwalsh@redhat.com>
