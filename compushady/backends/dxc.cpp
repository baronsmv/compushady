#include <Python.h>
#include <vector>
#include <dlfcn.h>

#include "dxcapi.h"
#include "compushady.h"

static PyObject *dxc_generate_exception(HRESULT hr, const char *prefix) {
    return PyErr_Format(PyExc_Exception, "%s: error code %d\n", prefix, hr);
}

static PyObject *dxc_compile(PyObject *self, PyObject *args) {
    Py_buffer view;
    PyObject *py_entry_point;
    int shader_binary_type;   // ignored – we always output SPIR‑V
    PyObject *py_target;
    if (!PyArg_ParseTuple(args, "s*UiU", &view, &py_entry_point, &shader_binary_type, &py_target))
        return NULL;

    static DxcCreateInstanceProc dxcompiler_lib_create_instance_proc = nullptr;
    if (!dxcompiler_lib_create_instance_proc) {
        dxcompiler_lib_create_instance_proc = (DxcCreateInstanceProc)dlsym(RTLD_DEFAULT, "DxcCreateInstance");
        if (!dxcompiler_lib_create_instance_proc)
            return PyErr_Format(PyExc_Exception, "unable to load dxcompiler library");
    }

    // Create DXC library
    IDxcLibrary *dxc_library = nullptr;
    HRESULT hr = dxcompiler_lib_create_instance_proc(CLSID_DxcLibrary, __uuidof(IDxcLibrary), (void **)&dxc_library);
    if (hr != S_OK)
        return dxc_generate_exception(hr, "unable to create DXC library instance");

    // Create DXC compiler
    IDxcCompiler *dxc_compiler = nullptr;
    hr = dxcompiler_lib_create_instance_proc(CLSID_DxcCompiler, __uuidof(IDxcCompiler), (void **)&dxc_compiler);
    if (hr != S_OK) {
        dxc_library->Release();
        return dxc_generate_exception(hr, "unable to create DXC compiler instance");
    }

    // Create blob from source
    IDxcBlobEncoding *blob_source = nullptr;
    hr = dxc_library->CreateBlobWithEncodingOnHeapCopy(view.buf, (UINT32)view.len, CP_UTF8, &blob_source);
    if (hr != S_OK) {
        dxc_compiler->Release();
        dxc_library->Release();
        return dxc_generate_exception(hr, "unable to create DXC blob");
    }

    // Convert Python strings to wide strings
    wchar_t *entry_point = PyUnicode_AsWideCharString(py_entry_point, nullptr);
    if (!entry_point) {
        blob_source->Release();
        dxc_compiler->Release();
        dxc_library->Release();
        return nullptr;
    }

    wchar_t *target = PyUnicode_AsWideCharString(py_target, nullptr);
    if (!target) {
        PyMem_Free(entry_point);
        blob_source->Release();
        dxc_compiler->Release();
        dxc_library->Release();
        return nullptr;
    }

    // Compiler arguments for SPIR‑V (Vulkan)
    std::vector<const wchar_t *> arguments = {
        L"-spirv",
        L"-fvk-auto-shift-bindings",
        L"-fvk-t-shift", L"1024", L"0",
        L"-fvk-u-shift", L"2048", L"0",
        L"-fvk-s-shift", L"3072", L"0",
        L"-fvk-use-dx-layout",
        L"-fvk-use-scalar-layout"
    };

    IDxcOperationResult *result = nullptr;
    hr = dxc_compiler->Compile(blob_source, nullptr, entry_point, target,
                               arguments.data(), (UINT32)arguments.size(),
                               nullptr, 0, nullptr, &result);

    PyMem_Free(target);
    PyMem_Free(entry_point);

    if (hr == S_OK)
        result->GetStatus(&hr);

    if (hr != S_OK) {
        if (result) {
            IDxcBlobEncoding *blob_error = nullptr;
            if (result->GetErrorBuffer(&blob_error) == S_OK) {
                PyObject *py_error = PyUnicode_FromStringAndSize(
                    (const char *)blob_error->GetBufferPointer(),
                    blob_error->GetBufferSize());
                PyErr_Format(PyExc_ValueError, "%U", py_error);
                Py_DECREF(py_error);
            }
            result->Release();
        }
        if (!PyErr_Occurred())
            dxc_generate_exception(hr, "Unable to compile HLSL shader");
        blob_source->Release();
        dxc_compiler->Release();
        dxc_library->Release();
        return nullptr;
    }

    IDxcBlob *compiled_blob = nullptr;
    result->GetResult(&compiled_blob);

    // Return SPIR‑V as bytes
    PyObject *py_compiled_blob = PyBytes_FromStringAndSize(
        (const char *)compiled_blob->GetBufferPointer(),
        compiled_blob->GetBufferSize());

    compiled_blob->Release();
    result->Release();
    blob_source->Release();
    dxc_compiler->Release();
    dxc_library->Release();

    return py_compiled_blob;
}

static PyMethodDef compushady_backends_dxc_methods[] = {
    {"compile", (PyCFunction)dxc_compile, METH_VARARGS, "Compile an HLSL shader to SPIR‑V"},
    {nullptr, nullptr, 0, nullptr}
};

static struct PyModuleDef compushady_backends_dxc_module = {
    PyModuleDef_HEAD_INIT, "dxc", nullptr, -1, compushady_backends_dxc_methods
};

PyMODINIT_FUNC PyInit_dxc(void) {
    return PyModule_Create(&compushady_backends_dxc_module);
}