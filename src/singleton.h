#pragma once

template <typename T>
class Singleton
{
public:
    struct object_creator
    {
        object_creator(){ Singleton<T>::instance(); }
        inline void do_nothing()const {}
    };
 
    static object_creator create_object;
 
    typedef T object_type;
 
    static object_type& instance()
    {
        static object_type obj;
        create_object.do_nothing();
        return obj;
    }
};

template <typename T> typename Singleton<T>::object_creator
Singleton<T>::create_object;