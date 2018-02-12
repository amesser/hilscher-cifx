Debianized Packet Source for cifX Communicatin cards
====================================================

This repository contains a debianzed version of the
the driver and libraries for cifX/netX communication cards
from Hilscher Gesellschaft für Systemautomation mbH.

This repository is not maintained by Hilscher Gesellschaft für
Systemautomation mbH. Unfortunately it is not allowed 
to publish the source code files. Therefore you
must purchase the original Linux cifX driver 
from Vendor in order to use build this package

In order to build the package, the following steps
are required:

* Checkout "suites/experimental" branch
* Get the BSL files from Vendor's Linux driver and 
  place them in driver/BSL/
* Get the libcifx files from Vendor's Linux driver 
  and place them in driver/libcifx/
* Invoke ``debuild -b`` to build the packages in 
  parent directory


Contact: Andreas Messer <andi@bastelmap.de>

