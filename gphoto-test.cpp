#include <stdio.h>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <string>
#include <chrono>
#include <cassert>
#include <cstring>

#include <gphoto2/gphoto2.h>


using namespace std::chrono_literals;

#define GP_CHECK(expr) { int c = (expr); _gphoto_check(#expr, c); }
static void _gphoto_check(const std::string &what, const int code)
{
    if(code < GP_OK)
    {
        printf("gphoto2 error: %s failed: %s (%d)\n", what.c_str(), gp_result_as_string(code), code);
        abort();
    }
}

#define GP_RETRY(n, expr) { \
    int count = n; \
    int err = GP_OK; \
    do { \
        err = (expr); \
            if(err == GP_ERROR_CAMERA_BUSY) { \
                count--; \
                usleep(1000*100); \
            } else { \
                break; \
            } \
    } while(count >0); \
    _gphoto_check(#expr, err); \
}

static void _gphoto_log(GPLogLevel level, const char *domain, const char *str, void */*data*/)
{
    if(!strcmp(domain, "ptp"))
        return;

    if(level == GP_LOG_ERROR)
        printf("libgphoto2[%s]: %s\n", domain, str);
    else if(level == GP_LOG_VERBOSE)
        printf("libgphoto2[%s]: %s\n", domain, str);
    else if(level == GP_LOG_DEBUG)
        printf("libgphoto2[%s]: %s\n", domain, str);
}

GPContext *context = nullptr;
Camera *cam = nullptr;

void config_set_int_value(const std::string &name, const CameraWidgetType type, const int value)
{
    CameraWidget *w = nullptr;
    GP_CHECK(gp_widget_new(type, name.c_str(), &w));
    GP_CHECK(gp_widget_set_value(w, &value));
    GP_RETRY(10, gp_camera_set_single_config(cam, name.c_str(), w, context));
    GP_CHECK(gp_widget_free(w));
}

void config_set_string_value(const std::string &name, const CameraWidgetType type, const std::string &value)
{
    CameraWidget *w = nullptr;
    GP_CHECK(gp_widget_new(type, name.c_str(), &w));
    GP_CHECK(gp_widget_set_value(w, value.c_str()));
    GP_RETRY(10, gp_camera_set_single_config(cam, name.c_str(), w, context));
    GP_CHECK(gp_widget_free(w));
}

std::string config_get_string_value(const std::string &name)
{
    std::string out;
    CameraWidget *w = nullptr;
    GP_CHECK(gp_camera_get_single_config(cam, name.c_str(), &w, context));
    CameraWidgetType w_type;
    GP_CHECK(gp_widget_get_type(w, &w_type));
    assert(w_type == GP_WIDGET_TEXT || w_type == GP_WIDGET_MENU || w_type == GP_WIDGET_RADIO);

    char *value;
    GP_CHECK(gp_widget_get_value(w, &value));
    if(value)
        out = std::string(value);
    GP_CHECK(gp_widget_free(w));

    return out;
}

int config_get_int_str_value(const std::string &name)
{
    int out = 0;
    CameraWidget *w = nullptr;
    GP_CHECK(gp_camera_get_single_config(cam, name.c_str(), &w, context));
    CameraWidgetType w_type;
    GP_CHECK(gp_widget_get_type(w, &w_type));
    assert(w_type == GP_WIDGET_RADIO || w_type == GP_WIDGET_TEXT || w_type == GP_WIDGET_MENU);

    char *value;
    GP_CHECK(gp_widget_get_value(w, &value));
    bool ok;

    if(value)
    {
        char *valueend =nullptr;
        const int v = std::strtol(value, &valueend, 10);
        if(valueend != value)
            out = v;
    }
    GP_CHECK(gp_widget_free(w));

    return out;
}

int config_get_bool_value(const std::string &name)
{
    CameraWidget *w = nullptr;
    GP_CHECK(gp_camera_get_single_config(cam, name.c_str(), &w, context));
    CameraWidgetType w_type;
    GP_CHECK(gp_widget_get_type(w, &w_type));
    assert(w_type == GP_WIDGET_TOGGLE);

    int value;
    GP_CHECK(gp_widget_get_value(w, &value));
    GP_CHECK(gp_widget_free(w));

    return value > 0;
}

int main(int, char**)
{
    context = gp_context_new();

    GPPortInfoList *port_info_list = nullptr;
    CameraAbilitiesList *camera_abilities_list = nullptr;

    GP_CHECK(gp_port_info_list_new(&port_info_list));
    GP_CHECK(gp_port_info_list_load(port_info_list));

    GP_CHECK(gp_abilities_list_new(&camera_abilities_list));
    GP_CHECK(gp_abilities_list_load(camera_abilities_list, context));

    CameraList *cam_list;
    GP_CHECK(gp_list_new(&cam_list));
    int num_cams = gp_camera_autodetect(cam_list, context);
    for(int i=0; i<num_cams; ++i)
    {
        const char *name, *port;
        gp_list_get_name(cam_list, i, &name);
        gp_list_get_value(cam_list, i, &port);

        GP_CHECK(gp_camera_new(&cam));

        CameraAbilities abilities;
        /* First lookup the model / driver */
        int model_index = gp_abilities_list_lookup_model(camera_abilities_list, name);
        _gphoto_check("gp_abilities_list_lookup_model", model_index);
        GP_CHECK(gp_abilities_list_get_abilities(camera_abilities_list, model_index, &abilities));
        GP_CHECK(gp_camera_set_abilities(cam, abilities));

        /* Then associate the camera with the specified port */
        int port_index = gp_port_info_list_lookup_path(port_info_list, port);
        _gphoto_check("gp_port_info_list_lookup_path", port_index);

        GPPortInfo port_info;

        GP_CHECK(gp_port_info_list_get_info(port_info_list, port_index, &port_info));
        GP_CHECK(gp_camera_set_port_info(cam, port_info));

        GP_CHECK(gp_camera_init(cam, context));
        break;
    }
    GP_CHECK(gp_list_unref(cam_list));

    int logfunc = gp_log_add_func(GP_LOG_DEBUG, _gphoto_log, 0);
    //config_set_string_value("controlmode", GP_WIDGET_RADIO, "Camera");

    int n=0;

    CameraEventType eventtype;
    void *eventdata;
    auto _clean_eventdata = [&eventdata]
    {
        if(eventdata)
        {
            free(eventdata);
            eventdata = nullptr;
        }
    };

    const std::chrono::time_point start = std::chrono::high_resolution_clock::now();

    int res;
    int photo = 0;
    std::chrono::time_point phototime = std::chrono::high_resolution_clock::now();
    while((res = gp_camera_wait_for_event(cam, 250, &eventtype, &eventdata, context)) >= GP_OK)
    {
        if(eventtype == GP_EVENT_FILE_ADDED)
        {
            CameraFile	*cam_file = nullptr;
            GP_CHECK(gp_file_new(&cam_file));

            CameraFilePath *cam_path = static_cast<CameraFilePath*>(eventdata);
            int res = gp_camera_file_get(cam, cam_path->folder, cam_path->name, GP_FILE_TYPE_NORMAL, cam_file, context);
            if (res < GP_OK)
            {
                gp_file_free(cam_file);
                _gphoto_check("gp_camera_file_get", res);
            }

            printf("Camera: File %s%s added\n", cam_path->folder, cam_path->name);

            unsigned long data_size;
            const char *data_buffer;
            res = gp_file_get_data_and_size(cam_file, &data_buffer, &data_size);
            if(res < GP_OK)
            {
                gp_file_free(cam_file);
                _gphoto_check("gp_file_get_data_and_size", res);
            }
            printf("Size: %lu bytes\n", data_size);

            gp_file_free(cam_file); // Invalidates data_buffer;
            //usleep(50*1000);

            GP_CHECK(gp_camera_file_delete(cam, cam_path->folder, cam_path->name, context));
            if(photo == 1)
            {
                phototime = std::chrono::high_resolution_clock::now();
                photo++;
            }
        }
        _clean_eventdata();
        const std::chrono::time_point now = std::chrono::high_resolution_clock::now();
        if(now > start + 1s && photo == 0)
        {
            photo++;
            config_set_string_value("prioritymode", GP_WIDGET_RADIO, "USB");
            //usleep(250*1000);
            gp_camera_trigger_capture(cam, context);
        }
        if(photo>=1)
        {
            printf("d212: %02x\n", config_get_bool_value("currentstate"));
        }
        if(now > phototime + 50ms && photo == 2)
        {
            config_set_string_value("prioritymode", GP_WIDGET_RADIO, "Camera");
            photo++;
        }
        if(now > start + 5s)
            break;
    }
    res += 0;

    GP_CHECK(gp_log_remove_func(logfunc));
    GP_CHECK(gp_camera_unref(cam));

    GP_CHECK(gp_abilities_list_free(camera_abilities_list));
    GP_CHECK(gp_port_info_list_free(port_info_list));

    gp_context_unref(context);
    return 0;
}
