/* dn_collector_c.cpp
 * Author: Jiayao Li
 * This file defines dm_collector_c, a Python extension module that collects
 * and decodes diagnositic logs from Qualcomm chipsets.
 */

#include <Python.h>

#include "consts.h"
#include "hdlc.h"
#include "log_config.h"
#include "log_packet.h"

#include <string>
#include <vector>
#include <algorithm>
#include <sys/time.h>
#include <iostream>
#include <sstream>

// NOTE: the following number should be updated every time.
#define DM_COLLECTOR_C_VERSION "1.0.11"


static PyObject *dm_collector_c_disable_logs (PyObject *self, PyObject *args);
static PyObject *dm_collector_c_enable_logs (PyObject *self, PyObject *args);
static PyObject *dm_collector_c_generate_diag_cfg (PyObject *self, PyObject *args);
static PyObject *dm_collector_c_feed_binary (PyObject *self, PyObject *args);
static PyObject *dm_collector_c_receive_log_packet (PyObject *self, PyObject *args);

static PyMethodDef DmCollectorCMethods[] = {
    {"disable_logs", dm_collector_c_disable_logs, METH_VARARGS,
        "Disable logs for a serial port.\n"
        "\n"
        "Args:\n"
        "    port: a diagnositic serial port.\n"
        "\n"
        "Returns:\n"
        "    Successful or not.\n"
    },
    {"enable_logs", dm_collector_c_enable_logs, METH_VARARGS,
        "Enable logs for a serial port.\n"
        "\n"
        "Args:\n"
        "    port: a diagnositic serial port.\n"
        "    type_names: a sequence of type names.\n"
        "\n"
        "Returns:\n"
        "    Successful or not.\n"
        "\n"
        "Raises\n"
        "    ValueError: when an unrecognized type name is passed in.\n"
    },
    {"feed_binary", dm_collector_c_feed_binary, METH_VARARGS,
        "Feed raw packets."},
    {"generate_diag_cfg", dm_collector_c_generate_diag_cfg, METH_VARARGS,
        "Generate a Diag.cfg file.\n"
        "\n"
        "This file can be loaded by diag_mdlog program on Android phones.\n"
    },
    {"receive_log_packet", dm_collector_c_receive_log_packet, METH_VARARGS,
        "Extract a log packet from feeded data.\n"
        "\n"
        "Args:\n"
        "    skip_decoding: If set to True, only the header would be decoded.\n"
        "        Default to False.\n"
        "    include_timestamp: Return the time when the message is received.\n"
        "        Default to False.\n"
        "\n"
        "Returns:\n"
        "    If include_timestamp is True, return (decoded, posix_timestamp);\n"
        "    otherwise only return decoded message.\n"
    },
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

// sort, unique and bucketing
static void
sort_type_ids(IdVector& type_ids, std::vector<IdVector>& out_vectors) {
    sort(type_ids.begin(), type_ids.end());
    IdVector::iterator itr = std::unique(type_ids.begin(), type_ids.end());
    type_ids.resize(std::distance(type_ids.begin(), itr));

    out_vectors.clear();
    int last_equip_id = -1;
    size_t i = 0, j = 0;
    for (j = 0; j < type_ids.size(); j++) {
        if (j != 0 && last_equip_id != get_equip_id(type_ids[j])) {
            out_vectors.push_back(IdVector(type_ids.begin() + i, 
                                            type_ids.begin() + j));
            i = j;
        }
        last_equip_id = get_equip_id(type_ids[j]);
    }
    if (i != j) {
        out_vectors.push_back(IdVector(type_ids.begin() + i, 
                                        type_ids.begin() + j));
    }
    return;
}

static bool
check_file (PyObject *o) {
    return PyObject_HasAttrString(o, "write");
}

// FIXME: currently it is the same with check_file(), but it should be stricter
static bool
check_serial_port (PyObject *o) {
    return PyObject_HasAttrString(o, "read");
}

static bool
send_msg (PyObject *serial_port, const char *b, int length) {
    std::string frame = encode_hdlc_frame(b, length);
    PyObject *o = PyObject_CallMethod(serial_port,
                                        (char *) "write",
                                        (char *) "s#", frame.c_str(), frame.size());
    Py_DECREF(o);
    return true;
}

static double
get_posix_timestamp () {
    struct timeval tv;
    (void) gettimeofday(&tv, NULL);
    return (double)(tv.tv_sec) + (double)(tv.tv_usec) / 1.0e6;
}

// Return: successful or not
static PyObject *
dm_collector_c_disable_logs (PyObject *self, PyObject *args) {
    IdVector empty;
    BinaryBuffer buf;
    PyObject *serial_port = NULL;
    if (!PyArg_ParseTuple(args, "O", &serial_port)) {
        return NULL;
    }
    Py_INCREF(serial_port);

    // Check arguments
    if (!check_serial_port(serial_port)) {
        PyErr_SetString(PyExc_TypeError, "\'port\' is not a serial port.");
        goto raise_exception;
    }

    buf = encode_log_config(DISABLE, empty);
    if (buf.first == NULL || buf.second == 0) {
        Py_DECREF(serial_port);
        Py_RETURN_FALSE;
    }
    (void) send_msg(serial_port, buf.first, buf.second);
    Py_DECREF(serial_port);
    delete [] buf.first;
    Py_RETURN_TRUE;

    raise_exception:
        Py_DECREF(serial_port);
        return NULL;
}

// A helper function that generated binary code to enable the specified types
// of messages.
// If error occurs, false is returned and PyErr_SetString() will be called.
static bool
generate_log_config_msgs (PyObject *file_or_serial, PyObject *type_names) {
    IdVector type_ids;
    std::vector<IdVector> type_id_vectors;
    BinaryBuffer buf;
    int n = PySequence_Length(type_names);

    for (int i = 0; i < n; i++) {
        PyObject *item = PySequence_GetItem(type_names, i);
        if (PyString_Check(item)) {
            const char *name = PyString_AsString(item);
            int cnt = find_ids(LogPacketTypeID_To_Name,
                                ARRAY_SIZE(LogPacketTypeID_To_Name, ValueName),
                                name, type_ids);
            if (cnt == 0) {
                PyErr_SetString(PyExc_ValueError, "Wrong type name.");
                Py_DECREF(item);
                return false;
            }
        } else {
            // ignore non-strings
        }
        if (item != NULL)
            Py_DECREF(item);    // Discard reference ownership
    }

    // Yuanjie: check if Modem_debug_message exists. If so, enable it in slightly different way
    IdVector::iterator debug_ind = type_ids.begin();
    for(; debug_ind != type_ids.end(); debug_ind++) {
        if(*debug_ind==Modem_debug_message){
            break;
        }
    }
    if(debug_ind!=type_ids.end()){
        //Modem_debug_message should be enabled
        type_ids.erase(debug_ind);
        buf = encode_log_config(DEBUG_LTE_ML1, type_ids);
        if (buf.first != NULL && buf.second != 0) {
            (void) send_msg(file_or_serial, buf.first, buf.second);
            delete [] buf.first;
        } else {
            PyErr_SetString(PyExc_RuntimeError, "Log config msg failed to encode.");
            return false;
        }
        buf = encode_log_config(DEBUG_WCDMA_L1, type_ids);
        if (buf.first != NULL && buf.second != 0) {
            (void) send_msg(file_or_serial, buf.first, buf.second);
            delete [] buf.first;
        } else {
            PyErr_SetString(PyExc_RuntimeError, "Log config msg failed to encode.");
            return false;
        }

    }

    // send log config messages
    sort_type_ids(type_ids, type_id_vectors);
    for (size_t i = 0; i < type_id_vectors.size(); i++) {
        const IdVector& v = type_id_vectors[i];
        buf = encode_log_config(SET_MASK, v);
        if (buf.first != NULL && buf.second != 0) {
            (void) send_msg(file_or_serial, buf.first, buf.second);
            delete [] buf.first;
        } else {
            PyErr_SetString(PyExc_RuntimeError, "Log config msg failed to encode.");
            return false;
        }
    }

    return true;
}

// Return: successful or not
static PyObject *
dm_collector_c_enable_logs (PyObject *self, PyObject *args) {
    PyObject *serial_port = NULL;
    PyObject *sequence = NULL;
    bool success = false;

    if (!PyArg_ParseTuple(args, "OO", &serial_port, &sequence)) {
        return NULL;
    }
    Py_INCREF(serial_port);
    Py_INCREF(sequence);

    // Check arguments
    if (!check_serial_port(serial_port)) {
        PyErr_SetString(PyExc_TypeError, "\'port\' is not a serial port.");
        goto raise_exception;
    }
    if (!PySequence_Check(sequence)) {
        PyErr_SetString(PyExc_TypeError, "\'type_names\' is not a sequence.");
        goto raise_exception;
    }

    success = generate_log_config_msgs(serial_port, sequence);
    if (!success) {
        goto raise_exception;
    }
    Py_DECREF(sequence);
    Py_DECREF(serial_port);
    Py_RETURN_TRUE;

    raise_exception:
        Py_DECREF(sequence);
        Py_DECREF(serial_port);
        return NULL;
}

// Return: successful or not
static PyObject *
dm_collector_c_generate_diag_cfg (PyObject *self, PyObject *args) {
    PyObject *file = NULL;
    PyObject *sequence = NULL;
    bool success = false;
    BinaryBuffer buf;
    IdVector empty;
 
    if (!PyArg_ParseTuple(args, "OO", &file, &sequence)) {
        return NULL;
    }
    Py_INCREF(file);
    Py_INCREF(sequence);

    // Check arguments
    if (!check_file(file)) {
        PyErr_SetString(PyExc_TypeError, "\'file\' is not a file object.");
        goto raise_exception;
    }
    if (!PySequence_Check(sequence)) {
        PyErr_SetString(PyExc_TypeError, "\'type_names\' is not a sequence.");
        goto raise_exception;
    }

    buf = encode_log_config(DISABLE, empty);
    if (buf.first != NULL && buf.second != 0) {
        (void) send_msg(file, buf.first, buf.second);
        delete [] buf.first;
    } else {
        PyErr_SetString(PyExc_RuntimeError, "Log config msg failed to encode.");
        goto raise_exception;
    }

    success = generate_log_config_msgs(file, sequence);
    if (!success) {
        goto raise_exception;
    }
    Py_DECREF(sequence);
    Py_DECREF(file);
    Py_RETURN_TRUE;

    raise_exception:
        Py_DECREF(sequence);
        Py_DECREF(file);
        return NULL;   
}

// Return: None
static PyObject *
dm_collector_c_feed_binary (PyObject *self, PyObject *args) {
    const char *b;
    int length;
    if (!PyArg_ParseTuple(args, "s#", &b, &length)){
        // printf("dm_collector_c_feed_binary returns NULL\n");
        return NULL;
    }
    feed_binary(b, length);
    Py_RETURN_NONE;
}

// Return: decoded_list or None
static PyObject *
dm_collector_c_receive_log_packet (PyObject *self, PyObject *args) {
    std::string frame;
    bool crc_correct = false;
    bool skip_decoding = false, include_timestamp = false;  // default values
    PyObject *arg_skip_decoding = NULL;
    PyObject *arg_include_timestamp = NULL;
    if (!PyArg_ParseTuple(args, "|OO:receive_log_packet",
                                &arg_skip_decoding, &arg_include_timestamp))
        return NULL;
    if (arg_skip_decoding != NULL) {
        Py_INCREF(arg_skip_decoding);
        skip_decoding = (PyObject_IsTrue(arg_skip_decoding) == 1);
        Py_DECREF(arg_skip_decoding);
    }
    if (arg_include_timestamp != NULL) {
        Py_INCREF(arg_include_timestamp);
        include_timestamp = (PyObject_IsTrue(arg_include_timestamp) == 1);
        Py_DECREF(arg_include_timestamp);
    }
    // printf("skip_decoding=%d, include_timestamp=%d\n", skip_decoding, include_timestamp);
    double posix_timestamp = (include_timestamp? get_posix_timestamp(): -1.0);

    bool success = get_next_frame(frame, crc_correct);
    // printf("success=%d crc_correct=%d is_log_packet=%d\n", success, crc_correct, is_log_packet(frame.c_str(), frame.size()));
    // if (success && crc_correct && is_log_packet(frame.c_str(), frame.size())) {
    if (success && crc_correct) {

        if(is_log_packet(frame.c_str(), frame.size())){
            const char *s = frame.c_str();
            PyObject *decoded = decode_log_packet(  s + 2,  // skip first two bytes
                                                    frame.size() - 2,
                                                    skip_decoding);
            if (include_timestamp) {
                PyObject *ret = Py_BuildValue("(Od)", decoded, posix_timestamp);
                Py_DECREF(decoded);
                return ret;
            } else {
                return decoded;
            }

        }
        else if(is_debug_packet(frame.c_str(), frame.size())){
            //Yuanjie: the original debug msg does not have header...
            char tmp[14]={
                0x00, 0x00,
                0x00, 0x00, 0xeb, 0x1f,
                0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00
            };
            tmp[2]=(char)(frame.size()+14);
            char *s = new char[14+frame.size()];
            memcpy(s,tmp,14);
            memcpy(s+14,frame.c_str(),frame.size());

            PyObject *decoded = decode_log_packet(s, frame.size()+14, skip_decoding);
            if (include_timestamp) {
                PyObject *ret = Py_BuildValue("(Od)", decoded, posix_timestamp);
                Py_DECREF(decoded);
                delete [] s;
                return ret;
            } else {
                delete [] s;
                return decoded;
            }
        }
        else {
            Py_RETURN_NONE;
        }
        
    } else {
        Py_RETURN_NONE;
    }
}

// Init the module
PyMODINIT_FUNC
initdm_collector_c(void)
{
    PyObject *dm_collector_c = Py_InitModule3("dm_collector_c", DmCollectorCMethods,
        "collects and decodes diagnositic logs from Qualcomm chipsets.");

    // dm_ccllector_c.log_packet_types: stores all supported type names
    int n_types = ARRAY_SIZE(LogPacketTypeID_To_Name, ValueName);
    PyObject *log_packet_types = PyTuple_New(n_types);
    for (int i = 0; i < n_types; i++) {
        // There is no leak here, because PyTuple_SetItem steals reference
        PyTuple_SetItem(log_packet_types,
                        i,
                        Py_BuildValue("s", LogPacketTypeID_To_Name[i].name));
    }
    PyObject_SetAttrString(dm_collector_c, "log_packet_types", log_packet_types);
    Py_DECREF(log_packet_types);

    // dm_ccllector_c.version: stores the value of DM_COLLECTOR_C_VERSION
    PyObject *pystr = PyString_FromString(DM_COLLECTOR_C_VERSION);
    PyObject_SetAttrString(dm_collector_c, "version", pystr);
    Py_DECREF(pystr);
}