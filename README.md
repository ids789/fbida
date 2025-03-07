[![Travis Status](https://travis-ci.com/kraxel/fbida.svg?branch=master)](https://travis-ci.com/gitlab/kraxel/fbida)
[![Copr Status](https://copr.fedorainfracloud.org/coprs/kraxel/mine.git/package/fbida/status_image/last_build.png)](https://copr.fedorainfracloud.org/coprs/kraxel/mine.git/package/fbida/)

# fbida

There a bunch of tools in this repository, bundled togethere here
because they share code.  Two of them ("fbi" and "ida") form the repo
name.

So, what is in there?

### ida

Motif image viewer, see [README.ida](README.ida)

### fbi

Linux console image viewer.  Runs on linux framebuffer devices (thus
the name "fbi").  Can also use drm devices.

### fbpdf

Linux console pdf viewer.  These days it is a standalone app using the
the poppler library for pdf rendering.

Originally it started as shell script which ran pdf/ps files through
ghostscript to render the pdf into a stack of image files for fbi.

### exiftran

Transform jpeg images, like jpegtran, but unlike jpegtran it also
transforms the exif thumbnail images.


------------
### Fork

This is a fork of fbida to add the following to fbi:
* automatically rotate images to match the orientation defined in the EXIF metadata
* add GPS cooridinates to the list of EXIF metadata values shown