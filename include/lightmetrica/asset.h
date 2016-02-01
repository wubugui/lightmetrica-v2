/*
    Lightmetrica - A modern, research-oriented renderer

    Copyright (c) 2015 Hisanari Otsu

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

#pragma once

#include <lightmetrica/component.h>
#include <lightmetrica/math.h>
#include <lightmetrica/spectrum.h>

LM_NAMESPACE_BEGIN

class Assets;
class PropertyNode;
struct SurfaceGeometry;
struct Primitive;

/*!
    \brief Asset.

    The base class of the asset classes.
    The `asset` is an important concept in the framework.
    All user-defined resources such as triangle meshes or BSDFs must inherits this class.
    The construction of assets are fully automated with asset management class (`Assets` class),
    which make it possible to extend your own assets consistently.
    For the design of the asset management in Lightmetrica, see <TODO>.
*/
class Asset : public Clonable
{
public:

    LM_INTERFACE_CLASS(Asset, Clonable);

public:

    Asset() = default;
    LM_DISABLE_COPY_AND_MOVE(Asset);

public:

    LM_INTERFACE_F(Load, bool(const PropertyNode* prop, Assets* assets, const Primitive* primitive));

public:

    auto ID() const -> std::string { return id_.Get(); }
    auto SetID(const std::string& id) -> void { id_.Reset(id); }

private:

    Portable<std::string> id_;

public:

    LM_INTERFACE_CLASS_END();

};

LM_NAMESPACE_END
