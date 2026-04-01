#
# spec file for package suse-monad
#
# Local packaging for openSUSE Tumbleweed.
# Update License if you choose to redistribute the project under a real license.
#
Name:           suse-monad
Version:        1.0.0
Release:        0
Summary:        JSON-driven xMonad desktop provisioner for openSUSE Tumbleweed
License:        LicenseRef-Proprietary
Group:          System/Management
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc
BuildRequires:  make
Requires:       zypper
ExclusiveArch:  x86_64

%description
suse-monad is a JSON-driven provisioning tool focused on openSUSE Tumbleweed.
It installs and configures a practical xMonad-on-X11 desktop and reads its
runtime policy from /etc/suse-monad/suse-monad.json.

The packaged desktop policy stays honest about the graphical stack: xMonad is
installed and configured as an X11 window manager/session. Optional
Wayland-friendly applications can be installed through the JSON policy, but the
primary desktop session remains xMonad on X11.

%prep
%autosetup -n %{name}-%{version}

%build
%make_build CFLAGS="%{optflags} -std=c11 -Wall -Wextra -Wpedantic"

%check
./suse-monad --help >/dev/null

%install
install -Dm0755 suse-monad %{buildroot}%{_bindir}/suse-monad
install -Dm0644 suse-monad.json %{buildroot}%{_sysconfdir}/suse-monad/suse-monad.json
install -Dm0644 suse-monad.8 %{buildroot}%{_mandir}/man8/suse-monad.8

%files
%defattr(-,root,root,-)
%doc README.rpm
%{_bindir}/suse-monad
%dir %{_sysconfdir}/suse-monad
%config(noreplace) %{_sysconfdir}/suse-monad/suse-monad.json
%{_mandir}/man8/suse-monad.8*

%changelog
* Wed Apr 01 2026 Local Packager <you@example.invalid>
- Initial local RPM package for suse-monad
