# Copyright (c) 2002-2005, Network Appliance, Inc. All rights reserved.
# Copyright (c) 2007, Intel Corporation. All rights reserved.
#
# This Software is licensed under one of the following licenses:
#
# 1) under the terms of the "Common Public License 1.0" a copy of which is
#    in the file LICENSE.txt in the root directory. The license is also
#    available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/cpl.php.
#
# 2) under the terms of the "The BSD License" a copy of which is in the file
#    LICENSE2.txt in the root directory. The license is also available from
#    the Open Source Initiative, see
#    http://www.opensource.org/licenses/bsd-license.php.
#
# 3) under the terms of the "GNU General Public License (GPL) Version 2" a 
#    copy of which is in the file LICENSE3.txt in the root directory. The 
#    license is also available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/gpl-license.php.
#
# Licensee has the right to choose one of the above licenses.
#
# Redistributions of source code must retain the above copyright
# notice and one of the license notices.
#
# Redistributions in binary form must reproduce both the above copyright
# notice, one of the license notices in the documentation
# and/or other materials provided with the distribution.
#
#
# uDAT and uDAPL 2.0 Registry RPM SPEC file
#
# $Id: $
Name: dapl
Version: 2.0.26
Release: 1%{?dist}
Summary: A Library for userspace access to RDMA devices using OS Agnostic DAT APIs.

Group: System Environment/Libraries
License: Dual GPL/BSD/CPL
Url: http://openfabrics.org/
Source: http://www.openfabrics.org/downloads/%{name}/%{name}-%{version}.tar.gz
BuildRoot: %(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig
Requires(post): sed
Requires(post): coreutils

%description
Along with the OpenFabrics kernel drivers, libdat and libdapl provides a userspace
RDMA API that supports DAT 2.0 specification and IB transport extensions for
atomic operations and rdma write with immediate data.

%package devel
Summary: Development files for the libdat and libdapl libraries
Group: System Environment/Libraries

%description devel
Header files for libdat and libdapl library.

%package devel-static
Summary: Static development files for libdat and libdapl library
Group: System Environment/Libraries
 
%description devel-static
Static libraries for libdat and libdapl library.

%package utils
Summary: Test suites for uDAPL library
Group: System Environment/Libraries
Requires: %{name} = %{version}-%{release}

%description utils
Useful test suites to validate uDAPL library API's.

%prep
%setup -q

%build
%configure --enable-ext-type=ib 
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
make DESTDIR=%{buildroot} install
# remove unpackaged files from the buildroot
rm -f %{buildroot}%{_libdir}/*.la
rm -f %{buildroot}%{_sysconfdir}/*.conf

%clean
rm -rf %{buildroot}

%post 
/sbin/ldconfig
if [ -e %{_sysconfdir}/dat.conf ]; then
    sed -e '/ofa-v2-.* u2/d' < %{_sysconfdir}/dat.conf > /tmp/$$ofadapl
    mv /tmp/$$ofadapl %{_sysconfdir}/dat.conf
fi
echo ofa-v2-mlx4_0-1 u2.0 nonthreadsafe default libdaploscm.so.2 dapl.2.0 '"mlx4_0 1" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-mlx4_0-2 u2.0 nonthreadsafe default libdaploscm.so.2 dapl.2.0 '"mlx4_0 2" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-ib0 u2.0 nonthreadsafe default libdaplofa.so.2 dapl.2.0 '"ib0 0" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-ib1 u2.0 nonthreadsafe default libdaplofa.so.2 dapl.2.0 '"ib1 0" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-mthca0-1 u2.0 nonthreadsafe default libdaploscm.so.2 dapl.2.0 '"mthca0 1" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-mthca0-2 u2.0 nonthreadsafe default libdaploscm.so.2 dapl.2.0 '"mthca0 2" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-ipath0-1 u2.0 nonthreadsafe default libdaploscm.so.2 dapl.2.0 '"ipath0 1" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-ipath0-2 u2.0 nonthreadsafe default libdaploscm.so.2 dapl.2.0 '"ipath0 2" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-ehca0-1 u2.0 nonthreadsafe default libdaploscm.so.2 dapl.2.0 '"ehca0 1" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-iwarp u2.0 nonthreadsafe default libdaplofa.so.2 dapl.2.0 '"eth2 0" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-mlx4_0-1u u2.0 nonthreadsafe default libdaploucm.so.2 dapl.2.0 '"mlx4_0 1" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-mlx4_0-2u u2.0 nonthreadsafe default libdaploucm.so.2 dapl.2.0 '"mlx4_0 2" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-mthca0-1u u2.0 nonthreadsafe default libdaploucm.so.2 dapl.2.0 '"mthca0 1" ""' >> %{_sysconfdir}/dat.conf
echo ofa-v2-mthca0-2u u2.0 nonthreadsafe default libdaploucm.so.2 dapl.2.0 '"mthca0 2" ""' >> %{_sysconfdir}/dat.conf

%postun 
/sbin/ldconfig
if [ -e %{_sysconfdir}/dat.conf ]; then
    sed -e '/ofa-v2-.* u2/d' < %{_sysconfdir}/dat.conf > /tmp/$$ofadapl
    mv /tmp/$$ofadapl %{_sysconfdir}/dat.conf
fi

%files
%defattr(-,root,root,-)
%{_libdir}/libda*.so.*
%doc AUTHORS README ChangeLog

%files devel
%defattr(-,root,root,-)
%{_libdir}/*.so
%dir %{_includedir}/dat2
%{_includedir}/dat2/*

%files devel-static
%defattr(-,root,root,-)
%{_libdir}/*.a

%files utils
%defattr(-,root,root,-)
%{_bindir}/*
%{_mandir}/man1/*.1*
%{_mandir}/man5/*.5*

%changelog
* Tue Jan 11 2010 Arlin Davis <ardavis@ichips.intel.com> - 2.0.26
- DAT/DAPL Version 2.0.26 Release 1, OFED 1.5, OFED 1.5-RDMAoE  

* Tue Nov 24 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.25
- DAT/DAPL Version 2.0.25 Release 1, OFED 1.5 RC3 

* Fri Oct 30 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.24
- DAT/DAPL Version 2.0.24 Release 1, OFED 1.5 RC2 

* Fri Oct 2 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.23
- DAT/DAPL Version 2.0.23 Release 1, OFED 1.5 RC1 

* Wed Aug 19 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.22
- DAT/DAPL Version 2.0.22 Release 1, OFED 1.5 ALPHA new UCM provider 

* Wed Aug 5 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.21
- DAT/DAPL Version 2.0.21 Release 1, WinOF 2.1, OFED 1.4.1+  

* Fri Jun 19 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.20
- DAT/DAPL Version 2.0.20 Release 1, OFED 1.4.1 + UD reject/scaling fixes 

* Thu Apr 30 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.19
- DAT/DAPL Version 2.0.19 Release 1, OFED 1.4.1 GA Final 

* Fri Apr 17 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.18
- DAT/DAPL Version 2.0.18 Release 1, OFED 1.4.1 GA 

* Tue Mar 31 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.17
- DAT/DAPL Version 2.0.17 Release 1, OFED 1.4.1 GA

* Mon Mar 16 2009 Arlin Davis <ardavis@ichips.intel.com> - 2.0.16
- DAT/DAPL Version 2.0.16 Release 1, OFED 1.4.1 

* Fri Nov 07 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.15
- DAT/DAPL Version 2.0.15 Release 1, OFED 1.4 GA

* Fri Oct 03 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.14
- DAT/DAPL Version 2.0.14 Release 1, OFED 1.4 rc3

* Mon Sep 01 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.13
- DAT/DAPL Version 2.0.13 Release 1, OFED 1.4 rc1

* Thu Aug 21 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.12
- DAT/DAPL Version 2.0.12 Release 1, OFED 1.4 beta

* Sun Jul 20 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.11
- DAT/DAPL Version 2.0.11 Release 1, IB UD extensions in SCM provider 

* Tue Jun 23 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.10
- DAT/DAPL Version 2.0.10 Release 1, socket CM provider 

* Tue May 20 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.9
- DAT/DAPL Version 2.0.9 Release 1, OFED 1.3.1 GA  

* Thu May 1 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.8
- DAT/DAPL Version 2.0.8 Release 1, OFED 1.3.1  

* Thu Feb 14 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.7
- DAT/DAPL Version 2.0.7 Release 1, OFED 1.3 GA 

* Mon Feb 04 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.6
- DAT/DAPL Version 2.0.6 Release 1, OFED 1.3 RC4

* Tue Jan 29 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.5
- DAT/DAPL Version 2.0.5 Release 1, OFED 1.3 RC3

* Thu Jan 17 2008 Arlin Davis <ardavis@ichips.intel.com> - 2.0.4
- DAT/DAPL Version 2.0.4 Release 1, OFED 1.3 RC2

* Tue Nov 20 2007 Arlin Davis <ardavis@ichips.intel.com> - 2.0.3
- DAT/DAPL Version 2.0.3 Release 1

* Tue Oct 30 2007 Arlin Davis <ardavis@ichips.intel.com> - 2.0.2
- DAT/DAPL Version 2.0.2 Release 1

* Tue Sep 18 2007 Arlin Davis <ardavis@ichips.intel.com> - 2.0.1-1
- OFED 1.3-alpha, co-exist with DAT 1.2 library package.  

* Wed Mar 7 2007 Arlin Davis <ardavis@ichips.intel.com> - 2.0.0.pre
- Initial release of DAT 2.0 APIs, includes IB extensions 
