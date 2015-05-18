%global chrony_version @@VERSION@@
%if 0%(echo %{chrony_version} | grep -q pre && echo 1)
%global prerelease %(echo %{chrony_version} | sed 's/.*-//')
%endif
Summary: An NTP client/server
Name: chrony
Version: %(echo %{chrony_version} | sed 's/-.*//')
Release: %{!?prerelease:1}%{?prerelease:0.1.%{prerelease}}
Source: chrony-%{version}%{?prerelease:-%{prerelease}}.tar.gz
License: GPLv2
Group: Applications/Utilities
BuildRoot: %{_tmppath}/%{name}-%{version}-root-%(id -u -n)
Requires: info

%description
chrony is a client and server for the Network Time Protocol (NTP).
This program keeps your computer's clock accurate. It was specially
designed to support systems with intermittent Internet connections,
but it also works well in permanently connected environments. It can
also use hardware reference clocks, the system real-time clock, or
manual input as time references.

%prep
%setup -q -n %{name}-%{version}%{?prerelease:-%{prerelease}}

%build
./configure \
	--prefix=%{_prefix} \
	--bindir=%{_bindir} \
	--sbindir=%{_sbindir} \
	--infodir=%{_infodir} \
	--mandir=%{_mandir}
make
make chrony.txt
make chrony.info

%install
rm -rf $RPM_BUILD_ROOT
make install DESTDIR=$RPM_BUILD_ROOT
rm -rf $RPM_BUILD_ROOT%{_docdir}
mkdir -p $RPM_BUILD_ROOT%{_infodir}
cp chrony.info* $RPM_BUILD_ROOT%{_infodir}

%files
%{_sbindir}/chronyd
%{_bindir}/chronyc
%{_infodir}/chrony.info*
%{_mandir}/man1/chronyc.1.gz
%{_mandir}/man5/chrony.conf.5.gz
%{_mandir}/man8/chronyd.8.gz
%doc README
%doc chrony.txt
%doc COPYING
%doc examples/chrony.conf.example*
%doc examples/chrony.keys.example

