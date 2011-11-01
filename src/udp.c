
#define UDP_MAX_BUF_SIZE 65536

static PyObject* PyExc_UDPConnectionError;


typedef struct {
    uv_udp_send_t req;
    uv_buf_t buf;
    void *data;
} udp_write_req_t;

typedef struct {
    PyObject *obj;
    PyObject *callback;
} udp_req_data_t;


static void
on_udp_close(uv_handle_t *handle)
{
    PyGILState_STATE gstate = PyGILState_Ensure();
    ASSERT(handle);
    /* Decrement reference count of the object this handle was keeping alive */
    PyObject *obj = (PyObject *)handle->data;
    Py_DECREF(obj);
    handle->data = NULL;
    PyMem_Free(handle);
    PyGILState_Release(gstate);
}


static uv_buf_t
on_udp_alloc(uv_udp_t* handle, size_t suggested_size)
{
    PyGILState_STATE gstate = PyGILState_Ensure();
    ASSERT(suggested_size <= UDP_MAX_BUF_SIZE);
    uv_buf_t buf;
    buf.base = PyMem_Malloc(suggested_size);
    buf.len = suggested_size;
    PyGILState_Release(gstate);
    return buf;
}


static void
on_udp_read(uv_udp_t* handle, int nread, uv_buf_t buf, struct sockaddr* addr, unsigned flags)
{
    PyGILState_STATE gstate = PyGILState_Ensure();

    char ip4[INET_ADDRSTRLEN];
    char ip6[INET6_ADDRSTRLEN];
    int r = 0;
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;

    PyObject *address_tuple;
    PyObject *data;
    PyObject *result;

    UNUSED_ARG(r);

    ASSERT(handle);
    ASSERT(flags == 0);

    UDPConnection *self = (UDPConnection *)handle->data;
    ASSERT(self);
    /* Object could go out of scope in the callback, increase refcount to avoid it */
    Py_INCREF(self);

    if (nread > 0) {
        ASSERT(addr);
        if (addr->sa_family == AF_INET) {
            addr4 = *(struct sockaddr_in*)addr;
            r = uv_ip4_name(&addr4, ip4, INET_ADDRSTRLEN);
            ASSERT(r == 0);
            address_tuple = Py_BuildValue("(si)", ip4, ntohs(addr4.sin_port));
        } else {
            addr6 = *(struct sockaddr_in6*)addr;
            r = uv_ip6_name(&addr6, ip6, INET6_ADDRSTRLEN);
            ASSERT(r == 0);
            address_tuple = Py_BuildValue("(si)", ip6, ntohs(addr6.sin6_port));
        }

        data = PyString_FromStringAndSize(buf.base, nread);

        result = PyObject_CallFunctionObjArgs(self->on_read_cb, self, address_tuple, data, NULL);
        if (result == NULL) {
            PyErr_WriteUnraisable(self->on_read_cb);
        }
        Py_XDECREF(result);
    } else {
        ASSERT(addr == NULL);
    }

    PyMem_Free(buf.base);

    Py_DECREF(self);
    PyGILState_Release(gstate);
}


static void
on_udp_write(uv_udp_send_t* req, int status)
{
    PyGILState_STATE gstate = PyGILState_Ensure();
    ASSERT(req);
    ASSERT(status == 0);

    udp_write_req_t *wr = (udp_write_req_t *)req;
    udp_req_data_t* req_data = (udp_req_data_t *)wr->data;

    UDPConnection *self = (UDPConnection *)req_data->obj;
    PyObject *callback = req_data->callback;

    ASSERT(self);
    /* Object could go out of scope in the callback, increase refcount to avoid it */
    Py_INCREF(self);
  
    PyObject *result;
    if (callback != Py_None) {
        result = PyObject_CallFunctionObjArgs(callback, self, NULL);
        if (result == NULL) {
            PyErr_WriteUnraisable(callback);
        }
        Py_XDECREF(result);
    }

    Py_DECREF(callback);
    PyMem_Free(req_data);
    wr->data = NULL;
    PyMem_Free(wr);

    Py_DECREF(self);
    PyGILState_Release(gstate);
}


static PyObject *
UDPConnection_func_bind(UDPConnection *self, PyObject *args)
{
    int r = 0;
    char *bind_ip;
    int bind_port;
    int address_type;
    struct in_addr addr4;
    struct in6_addr addr6;
    uv_udp_t *uv_udp_handle = NULL;

    if (self->bound) {
        PyErr_SetString(PyExc_UDPConnectionError, "already bound!");
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "(si):bind", &bind_ip, &bind_port)) {
        return NULL;
    }

    if (bind_port < 0 || bind_port > 65536) {
        PyErr_SetString(PyExc_ValueError, "port must be between 0 and 65536");
        return NULL;
    }

    if (inet_pton(AF_INET, bind_ip, &addr4) == 1) {
        address_type = AF_INET;
    } else if (inet_pton(AF_INET6, bind_ip, &addr6) == 1) {
        address_type = AF_INET6;
    } else {
        PyErr_SetString(PyExc_ValueError, "invalid IP address");
        return NULL;
    }

    uv_udp_handle = PyMem_Malloc(sizeof(uv_udp_t));
    if (!uv_udp_handle) {
        PyErr_NoMemory();
        goto error;
    }
    r = uv_udp_init(SELF_LOOP, uv_udp_handle);
    if (r != 0) {
        raise_uv_exception(self->loop, PyExc_UDPConnectionError);
        goto error;
    }
    uv_udp_handle->data = (void *)self;
    self->uv_udp_handle = uv_udp_handle;

    if (address_type == AF_INET) {
        r = uv_udp_bind(self->uv_udp_handle, uv_ip4_addr(bind_ip, bind_port), 0);
    } else {
        r = uv_udp_bind6(self->uv_udp_handle, uv_ip6_addr(bind_ip, bind_port), UV_UDP_IPV6ONLY);
    }

    if (r != 0) {
        raise_uv_exception(self->loop, PyExc_UDPConnectionError);
        return NULL;
    }

    self->bound = True;

    /* Increment reference count while libuv keeps this object around. It'll be decremented on handle close. */
    Py_INCREF(self);

    Py_RETURN_NONE;

error:
    if (uv_udp_handle) {
        uv_udp_handle->data = NULL;
        PyMem_Free(uv_udp_handle);
    }
    self->uv_udp_handle = NULL;
    return NULL;
}


static PyObject *
UDPConnection_func_start_read(UDPConnection *self, PyObject *args)
{
    int r = 0;
    PyObject *tmp = NULL;
    PyObject *callback;

    if (!self->bound) {
        PyErr_SetString(PyExc_UDPConnectionError, "not bound");
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "O:start_reading", &callback)) {
        return NULL;
    }

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "a callable is required");
        return NULL;
    } else {
        tmp = self->on_read_cb;
        Py_INCREF(callback);
        self->on_read_cb = callback;
        Py_XDECREF(tmp);
    }

    r = uv_udp_recv_start(self->uv_udp_handle, (uv_alloc_cb)on_udp_alloc, (uv_udp_recv_cb)on_udp_read);
    if (r != 0) {
        raise_uv_exception(self->loop, PyExc_UDPConnectionError);
        goto error;
    }

    Py_RETURN_NONE;

error:
    Py_DECREF(callback);
    return NULL;
}


static PyObject *
UDPConnection_func_stop_read(UDPConnection *self)
{
    if (!self->bound) {
        PyErr_SetString(PyExc_UDPConnectionError, "not bound");
        return NULL;
    }

    int r = uv_udp_recv_stop(self->uv_udp_handle);
    if (r) {
        raise_uv_exception(self->loop, PyExc_UDPConnectionError);
        return NULL;
    }
    Py_RETURN_NONE;
}


static PyObject *
UDPConnection_func_write(UDPConnection *self, PyObject *args)
{
    int r;
    char *write_data;
    char *dest_ip;
    int dest_port;
    int address_type;
    struct in_addr addr4;
    struct in6_addr addr6;
    udp_write_req_t *wr = NULL;
    udp_req_data_t *req_data = NULL;
    PyObject *address_tuple;
    PyObject *callback = Py_None;

    if (!PyArg_ParseTuple(args, "sO|O:write", &write_data, &address_tuple, &callback)) {
        return NULL;
    }

    if (callback != Py_None && !PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "a callable or None is required");
        return NULL;
    }

    if (!PyArg_ParseTuple(address_tuple, "si", &dest_ip, &dest_port)) {
        return NULL;
    }

    if (dest_port < 0 || dest_port > 65536) {
        PyErr_SetString(PyExc_ValueError, "port must be between 0 and 65536");
        return NULL;
    }

    if (inet_pton(AF_INET, dest_ip, &addr4) == 1) {
        address_type = AF_INET;
    } else if (inet_pton(AF_INET6, dest_ip, &addr6) == 1) {
        address_type = AF_INET6;
    } else {
        PyErr_SetString(PyExc_ValueError, "invalid IP address");
        return NULL;
    }

    wr = (udp_write_req_t*) PyMem_Malloc(sizeof(udp_write_req_t));
    if (!wr) {
        PyErr_NoMemory();
        goto error;
    }

    req_data = (udp_req_data_t*) PyMem_Malloc(sizeof(udp_req_data_t));
    if (!req_data) {
        PyErr_NoMemory();
        goto error;
    }

    wr->buf.base = write_data;
    wr->buf.len = strlen(write_data);

    req_data->obj = (PyObject *)self;
    Py_INCREF(callback);
    req_data->callback = callback;

    wr->data = (void *)req_data;

    if (address_type == AF_INET) {
        r = uv_udp_send(&wr->req, self->uv_udp_handle, &wr->buf, 1, uv_ip4_addr(dest_ip, dest_port), (uv_udp_send_cb)on_udp_write);
    } else {
        r = uv_udp_send6(&wr->req, self->uv_udp_handle, &wr->buf, 1, uv_ip6_addr(dest_ip, dest_port), (uv_udp_send_cb)on_udp_write);
    }
    if (r != 0) {
        raise_uv_exception(self->loop, PyExc_UDPConnectionError);
        goto error;
    }
    Py_RETURN_NONE;

error:
    if (req_data) {
        Py_DECREF(callback);
        req_data->obj = NULL;
        req_data->callback = NULL;
        PyMem_Free(req_data);
    }
    if (wr) {
        wr->data = NULL;
        PyMem_Free(wr);
    }
    return NULL;
}


static PyObject *
UDPConnection_func_close(UDPConnection *self)
{
    self->bound = False;
    if (self->uv_udp_handle != NULL) {
        uv_close((uv_handle_t *)self->uv_udp_handle, on_udp_close);
    }
    Py_RETURN_NONE;
}


static PyObject *
UDPConnection_func_getsockname(UDPConnection *self)
{
    struct sockaddr sockname;
    struct sockaddr_in *addr4;
    struct sockaddr_in6 *addr6;
    char ip4[INET_ADDRSTRLEN];
    char ip6[INET6_ADDRSTRLEN];
    int namelen = sizeof(sockname);

    if (!self->bound) {
        PyErr_SetString(PyExc_UDPConnectionError, "not bound");
        return NULL;
    }

    int r = uv_udp_getsockname(self->uv_udp_handle, &sockname, &namelen);
    if (r != 0) {
        raise_uv_exception(self->loop, PyExc_UDPConnectionError);
        return NULL;
    }

    if (sockname.sa_family == AF_INET) {
        addr4 = (struct sockaddr_in*)&sockname;
        r = uv_ip4_name(addr4, ip4, INET_ADDRSTRLEN);
        ASSERT(r == 0);
        return Py_BuildValue("si", ip4, ntohs(addr4->sin_port));
    } else if (sockname.sa_family == AF_INET6) {
        addr6 = (struct sockaddr_in6*)&sockname;
        r = uv_ip6_name(addr6, ip6, INET6_ADDRSTRLEN);
        ASSERT(r == 0);
        return Py_BuildValue("si", ip6, ntohs(addr6->sin6_port));
    } else {
        PyErr_SetString(PyExc_UDPConnectionError, "unknown address type detected");
        return NULL;
    }
}


static int
UDPConnection_tp_init(UDPConnection *self, PyObject *args, PyObject *kwargs)
{
    Loop *loop;
    PyObject *tmp = NULL;

    if (self->initialized) {
        PyErr_SetString(PyExc_UDPConnectionError, "Object already initialized");
        return -1;
    }

    if (!PyArg_ParseTuple(args, "O!:__init__", &LoopType, &loop)) {
        return -1;
    }

    tmp = (PyObject *)self->loop;
    Py_INCREF(loop);
    self->loop = loop;
    Py_XDECREF(tmp);

    self->initialized = True;
    self->bound = False;
    self->uv_udp_handle = NULL;

    return 0;
}


static PyObject *
UDPConnection_tp_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    UDPConnection *self = (UDPConnection *)PyType_GenericNew(type, args, kwargs);
    if (!self) {
        return NULL;
    }
    self->initialized = False;
    return (PyObject *)self;
}


static int
UDPConnection_tp_traverse(UDPConnection *self, visitproc visit, void *arg)
{
    Py_VISIT(self->on_read_cb);
    Py_VISIT(self->loop);
    return 0;
}


static int
UDPConnection_tp_clear(UDPConnection *self)
{
    Py_CLEAR(self->on_read_cb);
    Py_CLEAR(self->loop);
    return 0;
}


static void
UDPConnection_tp_dealloc(UDPConnection *self)
{
    UDPConnection_tp_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}


static PyMethodDef
UDPConnection_tp_methods[] = {
    { "bind", (PyCFunction)UDPConnection_func_bind, METH_VARARGS, "Bind to the specified IP and port." },
    { "start_read", (PyCFunction)UDPConnection_func_start_read, METH_VARARGS, "Start accepting data." },
    { "stop_read", (PyCFunction)UDPConnection_func_stop_read, METH_NOARGS, "Stop receiving data." },
    { "write", (PyCFunction)UDPConnection_func_write, METH_VARARGS, "Write data over UDP." },
    { "close", (PyCFunction)UDPConnection_func_close, METH_NOARGS, "Close UDP connection." },
    { "getsockname", (PyCFunction)UDPConnection_func_getsockname, METH_NOARGS, "Get local socket information." },
    { NULL }
};


static PyMemberDef UDPConnection_tp_members[] = {
    {"loop", T_OBJECT_EX, offsetof(UDPConnection, loop), READONLY, "Loop where this UDPConnection is running on."},
    {NULL}
};


static PyTypeObject UDPConnectionType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pyuv.UDPConnection",                                           /*tp_name*/
    sizeof(UDPConnection),                                          /*tp_basicsize*/
    0,                                                              /*tp_itemsize*/
    (destructor)UDPConnection_tp_dealloc,                           /*tp_dealloc*/
    0,                                                              /*tp_print*/
    0,                                                              /*tp_getattr*/
    0,                                                              /*tp_setattr*/
    0,                                                              /*tp_compare*/
    0,                                                              /*tp_repr*/
    0,                                                              /*tp_as_number*/
    0,                                                              /*tp_as_sequence*/
    0,                                                              /*tp_as_mapping*/
    0,                                                              /*tp_hash */
    0,                                                              /*tp_call*/
    0,                                                              /*tp_str*/
    0,                                                              /*tp_getattro*/
    0,                                                              /*tp_setattro*/
    0,                                                              /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE,  /*tp_flags*/
    0,                                                              /*tp_doc*/
    (traverseproc)UDPConnection_tp_traverse,                        /*tp_traverse*/
    (inquiry)UDPConnection_tp_clear,                                /*tp_clear*/
    0,                                                              /*tp_richcompare*/
    0,                                                              /*tp_weaklistoffset*/
    0,                                                              /*tp_iter*/
    0,                                                              /*tp_iternext*/
    UDPConnection_tp_methods,                                       /*tp_methods*/
    UDPConnection_tp_members,                                       /*tp_members*/
    0,                                                              /*tp_getsets*/
    0,                                                              /*tp_base*/
    0,                                                              /*tp_dict*/
    0,                                                              /*tp_descr_get*/
    0,                                                              /*tp_descr_set*/
    0,                                                              /*tp_dictoffset*/
    (initproc)UDPConnection_tp_init,                                /*tp_init*/
    0,                                                              /*tp_alloc*/
    UDPConnection_tp_new,                                           /*tp_new*/
};

