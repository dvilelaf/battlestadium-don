// Shim (T-601). The runtime build's header declares every generated function (about 191k lines, in generated/);
// 448 generated files call other generated functions directly and need those declarations. The module build
// will write the declarations for the files of its batch into a header of its own, found before this one.
// Left empty here: T-602 reports which sample files fail for lack of it.
#pragma once
