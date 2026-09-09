#pragma once
#include <memory>
#include <string>

#include <Python.h>

class Drawable;

// The Python type layer for the harvested drawables.
//
// McRogueFace puts each PyTypeObject in the same header as the class it wraps.
// Keeping it separate here means Drawable, Sprite, Caption, Texture, Font and
// Animation compile with no Python.h in sight -- the drawing core is a library
// that a CLI, a test harness or a different binding could sit on top of, which
// is the shape the rest of this project already has.
namespace PyDraw {

// Register clippy.Texture, .Font, .Drawable, .Sprite, .Caption, .Animation,
// the Easing and Align enums, and the module-level `stage`. Called from the
// clippy module's init function.
bool addToModule(PyObject* module, std::string& error_out);

// A wrapper for an existing drawable, of whichever concrete type it is.
// Returns a new reference, or nullptr with an exception set.
//
// Unlike McRogueFace, this does not preserve object identity: two lookups of
// the same drawable give two wrappers around one shared_ptr. Every attribute
// lives on the C++ side, so they behave identically -- but `a is b` is False
// where McRogueFace's PythonObjectCache would make it True.
PyObject* wrap(const std::shared_ptr<Drawable>& drawable);

} // namespace PyDraw
