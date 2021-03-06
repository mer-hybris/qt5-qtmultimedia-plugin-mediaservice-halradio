Name:       qt5-qtmultimedia-plugin-mediaservice-halradio

%{!?qtc_qmake:%define qtc_qmake %qmake}
%{!?qtc_qmake5:%define qtc_qmake5 %qmake5}
%{!?qtc_make:%define qtc_make make}
%{?qtc_builddir:%define _builddir %qtc_builddir}
Summary:    Qt Multimedia - HAL FM Radio media service
Version:    0.2.2
Release:    1%{?dist}
Group:      System/Libraries
License:    LGPLv2.1
URL:        https://github.com/mer-hybris/qt5-qtmultimedia-plugin-mediaservice-halradio
Source0:    %{name}-%{version}.tar.bz2
# Substitute with a UI app for your distribution:
Recommends: jolla-mediaplayer-radio
BuildRequires:  qt5-qtcore-devel
BuildRequires:  qt5-qmake
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Multimedia)
BuildRequires:  pkgconfig(android-headers)
BuildRequires:  pkgconfig(libhardware)
BuildRequires:  libhybris-devel

%description
Qt5 HAL FM Radio media service plugin


%prep
%setup -q -n %{name}-%{version}

%build

%qtc_qmake5

%qtc_make %{?_smp_mflags}

%install
rm -rf %{buildroot}
%qmake5_install

rm -f %{buildroot}/%{_libdir}/cmake/Qt5Multimedia/Qt5Multimedia_FMRadioServicePlugin.cmake

%files
%defattr(-,root,root,-)
%{_libdir}/qt5/plugins/mediaservice/libqtmedia_halradio.so
