#include <iostream>
#include <cmath>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include "baromesh/linkbot.hpp"
#include <boost/python.hpp>
#include <boost/filesystem.hpp>

using namespace boost::python;

class Linkbot : public barobo::Linkbot
{
    public:
    Linkbot(const std::string& serialId) 
        : barobo::Linkbot(serialId),
          m_jointStatesDirty(false)
    {
        if (! PyEval_ThreadsInitialized()) {
            PyEval_InitThreads();
        }
        /* Set up the joint event callback for moveWait() */
        barobo::Linkbot::setJointEventCallback(
                &Linkbot::jointEventCallback,
                this);

        barobo::FormFactor::Type formFactor;
        barobo::Linkbot::getFormFactor(formFactor);
        switch(formFactor) {
            case barobo::FormFactor::I:
                m_motorMask = 0x05;
                break;
            case barobo::FormFactor::L:
                m_motorMask = 0x03;
                break;
            case barobo::FormFactor::T:
                m_motorMask = 0x07;
                break;
        }
    }

    ~Linkbot()
    {
        if(!m_accelerometerEventCbObject.is_none()) {
            barobo::Linkbot::setAccelerometerEventCallback(nullptr, nullptr);
        }
        if(!m_buttonEventCbObject.is_none()) {
            barobo::Linkbot::setButtonEventCallback(nullptr, nullptr);
        }
        if(!m_encoderEventCbObject.is_none()) {
            barobo::Linkbot::setEncoderEventCallback(nullptr, 0, nullptr);
        }
        if(!m_jointEventCbObject.is_none()) {
            barobo::Linkbot::setJointEventCallback(nullptr, nullptr);
        }
        if(m_jointEventCbThread.joinable())
        {
            m_jointEventCbThread.join();
        }
        //std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    void releaseCallbacks()
    {
        m_buttonEventCbObject = boost::python::object();
        m_encoderEventCbObject = boost::python::object();
        m_jointEventCbObject = boost::python::object();
        m_accelerometerEventCbObject = boost::python::object();
    }

/* GETTERS */

    boost::python::tuple getAccelerometer() {
        int timestamp; 
        double x, y, z;
        barobo::Linkbot::getAccelerometer(timestamp, x, y, z);
        return boost::python::make_tuple(timestamp, x, y, z);
    }

    int getFormFactor() {
        barobo::FormFactor::Type form;
        barobo::Linkbot::getFormFactor(form);
        return form;
    }

    boost::python::tuple getJointAngles() {
        int timestamp;
        double j1, j2, j3;
        barobo::Linkbot::getJointAngles(timestamp, j1, j2, j3);
        return boost::python::make_tuple(timestamp, j1, j2, j3);
    }

    boost::python::tuple getJointSpeeds() {
        double j1, j2, j3;
        barobo::Linkbot::getJointSpeeds(j1, j2, j3);
        return boost::python::make_tuple(j1, j2, j3);
    }
    
    boost::python::tuple getJointStates() {
        int timestamp = 0;
        barobo::JointState::Type j1, j2, j3;
        barobo::Linkbot::getJointStates(timestamp, j1, j2, j3);
        boost::python::tuple rc;
        rc += boost::python::make_tuple(timestamp);
        for (auto i : {j1,j2,j3}) {
            rc += boost::python::make_tuple(static_cast<int>(i));
        }
        return rc;
    }
    
    boost::python::tuple getLedColor() {
        int r, g, b;
        barobo::Linkbot::getLedColor(r, g, b);
        return boost::python::make_tuple(r, g, b);
    }

    boost::python::tuple getVersions()
    {
        uint32_t v1, v2, v3;
        barobo::Linkbot::getVersions(v1, v2, v3);
        return boost::python::make_tuple(v1, v2, v3);
    }

/* MOVEMENT */

    void moveWait(int mask=0x07)
    {
        std::unique_lock<std::mutex> lock(m_jointStatesLock);
        int timestamp;
        barobo::Linkbot::getJointStates(timestamp, 
                       m_jointStates[0],
                       m_jointStates[1],
                       m_jointStates[2]);
        Py_BEGIN_ALLOW_THREADS
        bool waitrc = false;
        while(!waitrc) {
            waitrc = m_jointStatesCv.wait_for(lock, 
                    std::chrono::milliseconds(2000),
                    [this, mask] {
                        int timestamp;
                        if(!m_jointStatesDirty) {
                            /* We timed out. Get the current joint states */
                            barobo::Linkbot::getJointStates(timestamp, 
                                m_jointStates[0],
                                m_jointStates[1],
                                m_jointStates[2]);
                        }
                        bool moving = false;
                        int jointmask = 1;
                        for(auto& s : m_jointStates) {
                            if(mask&jointmask&m_motorMask) {
                                if(s == barobo::JointState::MOVING) {
                                    moving = true;
                                    break;
                                }
                            }
                            jointmask <<= 1;
                        }
                        m_jointStatesDirty = false;
                        return !moving;
                    }
            );
        }
        Py_END_ALLOW_THREADS
    }

/* CALLBACKS */

    void setButtonEventCallback(boost::python::object func)
    {
        static bool initialized = false;
        if(!initialized) {
            initialized = true;
            Py_DECREF(m_linkbot.ptr());
        }
        m_buttonEventCbObject = func;
        if(func.is_none()) {
            barobo::Linkbot::setButtonEventCallback(
                    nullptr, nullptr);
        } else {
            barobo::Linkbot::setButtonEventCallback(
                    &Linkbot::buttonEventCallback,
                    &m_buttonEventCbObject);
        }
    }

    static void buttonEventCallback(barobo::Button::Type buttonNo,
                                    barobo::ButtonState::Type event,
                                    int timestamp,
                                    void* userData)
    {
        std::thread cbThread( &Linkbot::buttonEventCallbackThread,
                              buttonNo,
                              event,
                              timestamp,
                              userData);
        cbThread.detach();
    }

    static void buttonEventCallbackThread(int buttonNo,
                                    barobo::ButtonState::Type event,
                                    int timestamp,
                                    void* userData)
    {
        /* Lock the Python GIL */
        PyGILState_STATE gstate;
        gstate = PyGILState_Ensure();

        /* The userData should be a python object */
        boost::python::object* func =
            static_cast<boost::python::object*>(userData);
        if(!func->is_none()) {
            (*func)(buttonNo, static_cast<int>(event), timestamp);
        }

        /* Release the Python GIL */
        PyGILState_Release(gstate);
    }

    void setEncoderEventCallback(boost::python::object func, float granularity)
    {
        m_encoderEventCbObject = func;
        if(func.is_none()) {
            barobo::Linkbot::setEncoderEventCallback(
                    nullptr, granularity, nullptr);
        } else {
            barobo::Linkbot::setEncoderEventCallback(
                    &Linkbot::encoderEventCallback,
                    granularity,
                    &m_encoderEventCbObject);
        }
    }

    static void encoderEventCallback(int jointNo,
                                     double anglePosition,
                                     int timestamp,
                                     void* userData)
    {
        std::thread cbThread ( &Linkbot::encoderEventCallbackThread,
                               jointNo,
                               anglePosition,
                               timestamp,
                               userData );
        cbThread.detach();
    }

    static void encoderEventCallbackThread(int jointNo,
                                    double anglePosition,
                                    int timestamp,
                                    void* userData)
    {
        /* Lock the Python GIL */
        PyGILState_STATE gstate;
        gstate = PyGILState_Ensure();

        /* The userData should be a python object */
        boost::python::object* func =
            static_cast<boost::python::object*>(userData);
        if(!func->is_none()) {
            (*func)(jointNo+1, anglePosition, timestamp);
        }

        /* Release the Python GIL */
        PyGILState_Release(gstate);
    }

    void setJointEventCallback(boost::python::object func)
    {
        m_jointEventCbObject = func;
    }

    static void jointEventCallback(int jointNo, 
                                   barobo::JointState::Type event,
                                   int timestamp,
                                   void* userData)
    {
        auto l = static_cast<Linkbot*>(userData);

        std::thread jointEvent (
            [l, jointNo, event] 
            {
                std::unique_lock<std::mutex> lock(l->m_jointStatesLock);
                l->m_jointStates[jointNo] = event;
                l->m_jointStatesDirty = true;
                l->m_jointStatesCv.notify_all();
                lock.unlock();
            });
        jointEvent.detach();
    
        if(!l->m_jointEventCbObject.is_none()) {
            if(l->m_jointEventCbThread.joinable())
                l->m_jointEventCbThread.join();
            std::thread cbThread ( &Linkbot::jointEventCallbackThread,
                    jointNo,
                    event,
                    timestamp,
                    userData);
            l->m_jointEventCbThread.swap(cbThread);
            if(cbThread.joinable())
                cbThread.join();
        }
    }

   static void jointEventCallbackThread(int jointNo,
                                   barobo::JointState::Type event,
                                   int timestamp,
                                   void* userData)
    {
        /* The userData should be a Linkbot object */
        auto l = static_cast<Linkbot*>(userData);
        auto &func = l->m_jointEventCbObject;
        
        if(!func.is_none()) {
            /* Lock the Python GIL */
            PyGILState_STATE gstate;
            gstate = PyGILState_Ensure();

            (func)(jointNo, static_cast<int>(event), timestamp);

            /* Release the Python GIL */
            PyGILState_Release(gstate);
        }
    }

    void setAccelerometerEventCallback(boost::python::object func)
    {
        m_accelerometerEventCbObject = func;
        if(func.is_none()) {
            barobo::Linkbot::setAccelerometerEventCallback(
                    nullptr, nullptr);
        } else {
            barobo::Linkbot::setAccelerometerEventCallback(
                    &Linkbot::accelerometerEventCallback,
                    &m_accelerometerEventCbObject);
        }
    }

    static void accelerometerEventCallback(double x,
                                           double y,
                                           double z,
                                           int timestamp,
                                           void* userData)
    {
        std::thread cbThread( &Linkbot::accelerometerEventCallbackThread,
                              x, y, z, timestamp, userData);
        cbThread.detach();
    }

    static void accelerometerEventCallbackThread(double x,
                                           double y,
                                           double z,
                                           int timestamp,
                                           void* userData)
    {
        /* Lock the Python GIL */
        PyGILState_STATE gstate;
        gstate = PyGILState_Ensure();

        /* The userData should be a python object */
        boost::python::object* func =
            static_cast<boost::python::object*>(userData);
        if(!func->is_none()) {
            (*func)(x, y, z, timestamp);
        }

        /* Release the Python GIL */
        PyGILState_Release(gstate);
    }

    /* MISC */

    void writeEeprom(int addr, boost::python::object buffer) {
        PyObject* py_buffer = buffer.ptr();
        /* FIXME: The next line should raise an exception in Python */
        if(!PyObject_CheckBuffer(py_buffer)) return;
        Py_buffer view;
        if(PyObject_GetBuffer(py_buffer, &view, 0)) {
            return;
        }
        barobo::Linkbot::writeEeprom( addr, 
                     static_cast<const uint8_t*>(view.buf), 
                     static_cast<size_t>(view.len));
        PyBuffer_Release(&view);
    }

    boost::python::list readEeprom(int addr, int size) {
        uint8_t buf[128];
        barobo::Linkbot::readEeprom(addr, size, buf);
        boost::python::list retval;
        for(int i = 0; i < size; i++) {
            retval.append(buf[i]);
        }
        return retval;
    }

    void writeTwi(int addr, boost::python::object buffer) {
        PyObject* py_buffer = buffer.ptr();
        /* FIXME: The next line should raise an exception in Python */
        if(!PyObject_CheckBuffer(py_buffer)) return;
        Py_buffer view;
        if(PyObject_GetBuffer(py_buffer, &view, 0)) {
            return;
        }
        barobo::Linkbot::writeTwi( addr, 
                     static_cast<const uint8_t*>(view.buf), 
                     static_cast<size_t>(view.len));
        PyBuffer_Release(&view);
    }

    boost::python::list readTwi(int addr, int size) {
        uint8_t buf[128];
        barobo::Linkbot::readTwi(addr, size, buf);
        boost::python::list retval;
        for(int i = 0; i < size; i++) {
            retval.append(buf[i]);
        }
        return retval;
    }

    boost::python::list writeReadTwi(int addr, boost::python::object sendbuf,
        int recvsize) {
        PyObject* py_buffer = sendbuf.ptr();
        /* FIXME: The next line should raise an exception in Python */
        if(!PyObject_CheckBuffer(py_buffer)) return {};
        Py_buffer view;
        if(PyObject_GetBuffer(py_buffer, &view, 0)) {
            return {};
        }
        uint8_t buf[128];
        std::cout<<"Sending "<<view.len<<" bytes."<<std::endl;
        std::cout<<"Receive "<<recvsize<<" bytes."<<std::endl;
        barobo::Linkbot::writeReadTwi( addr, 
                     static_cast<const uint8_t*>(view.buf), 
                     static_cast<size_t>(view.len),
                     buf,
                     recvsize);
        PyBuffer_Release(&view);
        boost::python::list retval;
        for(int i = 0; i < recvsize; i++) {
            retval.append(buf[i]);
        }
        return retval;
    }

    private:
        int m_motorMask;
        boost::python::object m_buttonEventCbObject;
        std::thread m_buttonEventCbThread;
        boost::python::object m_encoderEventCbObject;
        std::thread m_encoderEventCbThread;
        boost::python::object m_jointEventCbObject;
        std::thread m_jointEventCbThread;
        boost::python::object m_accelerometerEventCbObject;
        std::thread m_accelerometerEventCbThread;

        barobo::JointState::Type m_jointStates[3];
        bool m_jointStatesDirty;
        std::mutex m_jointStatesLock;
        std::condition_variable m_jointStatesCv;

        boost::python::object m_linkbot;
};

BOOST_PYTHON_MODULE(_linkbot)
{
    boost::filesystem::path::imbue(std::locale("C"));
    #define LINKBOT_FUNCTION(func, docstring) \
    .def(#func, &Linkbot::func, docstring)
    class_<Linkbot,boost::noncopyable>("Linkbot", init<const char*>())
        #include"linkbot_functions.x.h"
        .def("moveWait", &Linkbot::moveWait)
        .def("_releaseCallbacks", &Linkbot::releaseCallbacks)
        ;
    #undef LINKBOT_FUNCTION
}
