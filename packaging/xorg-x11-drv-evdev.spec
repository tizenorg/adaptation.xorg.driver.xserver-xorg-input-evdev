Name:       xorg-x11-drv-evdev
Summary:    Xorg X11 evdev input driver
Version:    2.3.2
Release:    4
Group:      System/X Hardware Support
License:    MIT
URL:        http://www.x.org/
Source0:    http://xorg.freedesktop.org/releases/individual/driver/xf86-input-evdev-%{version}.tar.gz
Requires:   xserver-xorg-core
BuildRequires:  pkgconfig(xorg-server)
BuildRequires:  pkgconfig(xkbfile)
BuildRequires:  pkgconfig(xproto)
BuildRequires:  pkgconfig(inputproto)
BuildRequires:  pkgconfig(xrandr)
BuildRequires:  pkgconfig(randrproto)
BuildRequires:  pkgconfig(xextproto)
BuildRequires:  pkgconfig(xorg-macros)


%description
The Xorg X11 evdev input driver


%package devel
Summary:    Development files for xorg evdev driver
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description devel
This package contains xorg evdev development files


%prep
%setup -q -n %{name}-%{version}

%build

%reconfigure --disable-static
make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install

%remove_docs

%files
%defattr(-,root,root,-)
%(pkg-config xorg-server --variable=moduledir )/input/evdev_drv.so

%files devel
%defattr(-,root,root,-)
%{_includedir}/xorg/evdev-properties.h
%{_libdir}/pkgconfig/xorg-evdev.pc

