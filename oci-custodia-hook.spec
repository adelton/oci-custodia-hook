
Name:           oci-custodia-hook
Version:        0.1.4
Release:        1%{?dist}
Summary:        OCI custodia hook for docker
Group:          Applications/Text
License:        GPLv3+
URL:            https://www.adelton.com/%{name}/
Source0:        https://www.adelton.com/%{name}/%{name}-%{version}.tar.gz

BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  pkgconfig(yajl)
BuildRequires:  pkgconfig(libselinux)
BuildRequires:  pkgconfig(mount)
BuildRequires:  golang-github-cpuguy83-go-md2man

%description
OCI custodia hook bind mounts custodia socket to containers.

%prep
%setup -q

%build
autoreconf -i
%configure --libexecdir=/usr/libexec/oci/hooks.d/
make %{?_smp_mflags}

%install
%make_install

#define license tag if not already defined
%{!?_licensedir:%global license %doc}
%files
%{_libexecdir}/oci/hooks.d/oci-custodia-hook
%{_mandir}/man1/oci-custodia-hook.1*
%doc README.md
%license LICENSE
%dir /%{_libexecdir}/oci
%dir /%{_libexecdir}/oci/hooks.d

%changelog
* Fri May 19 2017 Jan Pazdziora <jpazdzio@redhat.com> - 0.1.4-1
- Initial release.
- Essentially stripped down and modified source of oci-systemd-hook.
