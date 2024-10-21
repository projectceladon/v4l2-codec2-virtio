# DISCONTINUATION OF PROJECT #  
This project will no longer be maintained by Intel.  
Intel has ceased development and contributions including, but not limited to, maintenance, bug fixes, new releases, or updates, to this project.  
Intel no longer accepts patches to this project.  
 If you have an ongoing need to use this project, are interested in independently developing it, or would like to maintain patches for the open source software community, please create your own fork of this project.  
  
# V4L2-based Codec2 Component Implementation

## Description of Sub-folders

* accel/
Core V4L2 API and codec utilities, ported from Chromium project.

* common/
Common helper classes for both components/ and store/.

* components/
The C2Component implementations based on V4L2 API.

* store/
The implementation of C2ComponentStore. It is used for creating all the
C2Components implemented at components/ folder.

* service/
The Codec2's V4L2 IComponentStore service. The service initiates the component
store implemented at store/ folder, and registers it as the default service.
