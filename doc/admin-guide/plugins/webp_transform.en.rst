.. Licensed to the Apache Software Foundation (ASF) under one
   or more contributor license agreements.  See the NOTICE file
   distributed with this work for additional information
   regarding copyright ownership.  The ASF licenses this file
   to you under the Apache License, Version 2.0 (the
   "License"); you may not use this file except in compliance
   with the License.  You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing,
   software distributed under the License is distributed on an
   "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
   KIND, either express or implied.  See the License for the
   specific language governing permissions and limitations
   under the License.

.. include:: ../../common.defs

.. _admin-plugins-webp-transform:

Webp Transform Plugin
*********************

This plugin converts jpeg and png images to webp for clients that support webp,
and converts webp images to jpeg (or png, when the image has transparency) for
clients that don't. A client supports webp only if its ``Accept`` header lists
``image/webp`` explicitly; ``image/*`` and ``*/*`` are treated as jpeg, gif and
png only, as older browsers that sent them could not decode webp.

The response's ``Content-Type`` always matches the body that is sent.

Installation
============

Add the following line to :file:`plugin.config`::

    webp_transform.so [convert_to_jpeg] [convert_to_webp] [max_buffer_size=<size>]
                      [webp_quality=<1-100>] [jpeg_quality=<1-100>]


Plugin Arguments
================

The plugin is configured with space-separated arguments. All are optional.

``convert_to_webp``
    Convert ``image/jpeg`` and ``image/png`` responses to ``image/webp`` for
    clients that advertise ``image/webp`` in their ``Accept`` header.

``convert_to_jpeg``
    Convert ``image/webp`` responses to ``image/jpeg`` for clients that do not
    advertise ``image/webp``. A webp image with transparency is converted to
    ``image/png`` instead, since jpeg cannot represent it.

    If neither ``convert_to_webp`` nor ``convert_to_jpeg`` is given, both
    conversions are enabled.

``webp_quality=<1-100>``
    Encoder quality for webp output. The default is 80. Output is always
    lossy; the source image's quality is never carried over. If the webp is
    not smaller than the original jpeg or png, the original is sent instead.

``jpeg_quality=<1-100>``
    Encoder quality for jpeg output. The default is 85.

``max_buffer_size=<size>``
    The maximum size of a single response body the plugin will buffer in memory
    before handing it to ImageMagick, which bounds the memory a large image
    response can consume. ``<size>`` is a byte count with an optional ``K``,
    ``M``, or ``G`` suffix (1024-based), for example ``max_buffer_size=32M``.
    The default is 16 MiB.

    There are two distinct over-limit behaviors, depending on whether the size
    is known before the body is read:

    * If the origin advertises a ``Content-Length`` greater than the limit, the
      transform is declined up front and the original response is passed through
      to the client unchanged.

    * If the size is not known in advance (for example a chunked response with
      no usable ``Content-Length``), the body is buffered until it exceeds the
      limit. At that point the transform cannot complete and produces no body,
      so the client receives a ``502 Bad Gateway`` rather than a pass-through of
      the original bytes.


Note
====

This plugin only supports jpeg and png and requires Magick++ from ImageMagick.
Other image formats can easily be supported.

In addition to ``max_buffer_size``, the plugin sets fixed ImageMagick decode
resource limits (image dimensions, pixel-cache memory, and disk) so that a
small image declaring very large dimensions cannot decode into an oversized
pixel buffer. An image that exceeds those limits is passed through in its
original format rather than converted.
