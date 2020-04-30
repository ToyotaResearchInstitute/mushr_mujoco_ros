#include "mujoco.h"
#include "ros/ros.h"
#include "yaml-cpp/yaml.h"

#include "body_ros_connector.h"
#include "mjglobal.h"
#include "mushr_mujoco_util.h"
#include "mushr_ros_connector.h"
#include "rollout.h"
#include "simple_viz.h"

#include "mushr_mujoco_ros/BodyStateArray.h"
#include "mushr_mujoco_ros/GetState.h"
#include "mushr_mujoco_ros/Reset.h"
#include "mushr_mujoco_ros/Step.h"

void set_body_state(
    mjData* d,
    std::map<std::string, mushr_mujoco_ros::MuSHRROSConnector*>& car_conn,
    std::map<std::string, mushr_mujoco_ros::BodyROSConnector*>& body_conn,
    mushr_mujoco_ros::BodyStateArray& body_state)
{
    body_state.simtime = d->time;
    body_state.header.stamp = ros::Time::now();

    for (auto cc : car_conn)
    {
        mushr_mujoco_ros::BodyState bs;
        cc.second->set_body_state(bs);
        body_state.states.push_back(bs);
    }
    for (auto bc : body_conn)
    {
        mushr_mujoco_ros::BodyState bs;
        bc.second->set_body_state(bs);
        body_state.states.push_back(bs);
    }
}

class SrvResponder
{
  public:
    SrvResponder(
        std::map<std::string, mushr_mujoco_ros::MuSHRROSConnector*>& c,
        std::map<std::string, mushr_mujoco_ros::BodyROSConnector*>& b)
      : car_conn{c}, body_conn{b}
    {
    }

    bool step(
        mushr_mujoco_ros::Step::Request& req, mushr_mujoco_ros::Step::Response& res)
    {
        mjModel* m = mjglobal::mjmodel();
        mjData* d = mjglobal::mjdata_lock();
        float maxrate = 60.0;

        mjtNum vel = req.ctrl.drive.speed;
        mjtNum steering_angle = req.ctrl.drive.steering_angle;

        mjtNum simstart = d->time;

        /* ROS_INFO_THROTTLE(1, "simstart %f, vel %f, steering_angle %f", simstart, vel,
         * steering_angle); */
        /* set_body_state(d, car_conn, body_conn, res.body_state); */
        /* for (int i = 0; i < res.body_state.states.size(); i++) */
        /* { */
        /*     auto name = res.body_state.states[i].name; */
        /*     auto pose = res.body_state.states[i].pose; */
        /*     ROS_INFO_THROTTLE(1, "%s x %f, y %f, q(%f, %f, %f, %f)", */
        /*             name.c_str(), pose.position.x, pose.position.y, */
        /*             pose.orientation.w, pose.orientation.x, pose.orientation.y,
         * pose.orientation.z); */
        /* } */

        if (!mushr_mujoco_util::is_paused())
        {
            while (d->time - simstart < 1.0 / maxrate)
            {
                mj_step1(m, d);
                car_conn["buddy"]->apply_control(d, vel, steering_angle);
                mj_step2(m, d);
            }
        }

        set_body_state(d, car_conn, body_conn, res.body_state);
        mjglobal::mjdata_unlock();
        return true;
    }

    bool get_state(
        mushr_mujoco_ros::GetState::Request& req,
        mushr_mujoco_ros::GetState::Response& res)
    {
        mjData* d = mjglobal::mjdata_lock();
        set_body_state(d, car_conn, body_conn, res.body_state);
        mjglobal::mjdata_unlock();
        return true;
    }

    bool reset(
        mushr_mujoco_ros::Reset::Request& req, mushr_mujoco_ros::Reset::Response& res)
    {
        mjModel* m = mjglobal::mjmodel();
        mjData* d = mjglobal::mjdata_lock();

        ROS_INFO("Reset initiated");
        if (req.body_names.size() != req.init_state.size())
        {
            return false;
        }

        mushr_mujoco_util::reset(m, d);

        for (int i = 0; i < req.body_names.size(); i++)
        {
            auto car = car_conn.find(req.body_names[i]);
            if (car != car_conn.end())
            {
                car->second->set_pose(req.init_state[i]);
            }

            auto body = body_conn.find(req.body_names[i]);
            if (body != body_conn.end())
            {
                body->second->set_pose(req.init_state[i]);
            }
        }

        set_body_state(d, car_conn, body_conn, res.body_state);

        mjglobal::mjdata_unlock();
        return true;
    }

  private:
    std::map<std::string, mushr_mujoco_ros::MuSHRROSConnector*>& car_conn;
    std::map<std::string, mushr_mujoco_ros::BodyROSConnector*>& body_conn;
};

int main(int argc, char** argv)
{
    std::string model_file_path;
    bool do_viz;

    ros::init(argc, argv, "mushr_mujoco_ros");
    ros::NodeHandle nh("~");

    mushr_mujoco_util::init_mj(&nh);

    if (!nh.getParam("model_file_path", model_file_path))
    {
        ROS_FATAL("%s not set", nh.resolveName("model_file_path").c_str());
        exit(1);
    }
    if (!nh.getParam("viz", do_viz))
    {
        ROS_FATAL("%s not set", nh.resolveName("viz").c_str());
        exit(1);
    }

    ROS_INFO("Loading model");
    // make and model data
    char* error;
    if (mjglobal::init_model(model_file_path.c_str(), &error))
    {
        ROS_FATAL("Could not load binary model %s", error);
        exit(1);
    }
    ROS_INFO("Loading data");
    mjglobal::init_data();
    ROS_INFO("Loaded model and data");

    std::string config_file;
    if (!nh.getParam("config_file_path", config_file))
    {
        ROS_FATAL("%s not set", nh.resolveName("config_file_path").c_str());
        exit(1);
    }

    YAML::Node config;
    try
    {
        config = YAML::LoadFile(config_file);
    }
    catch (YAML::BadFile e)
    {
        ROS_INFO("Couldn't open file %s", config_file.c_str());
        exit(1);
    }
    catch (std::exception e)
    {
        ROS_INFO("Unknown exception opening config file");
        exit(1);
    }

    std::map<std::string, mushr_mujoco_ros::MuSHRROSConnector*> car_conn;
    std::map<std::string, mushr_mujoco_ros::BodyROSConnector*> body_conn;

    ROS_INFO("Loading car configuration");
    // Load car info
    if (config["cars"])
    {
        YAML::Node car_cfg = config["cars"];
        for (int i = 0; i < car_cfg.size(); i++)
        {
            auto mrc = new mushr_mujoco_ros::MuSHRROSConnector(&nh, car_cfg[i]);
            car_conn[mrc->body_name()] = mrc;
        }
    }

    ROS_INFO("Loading bodies configuration");
    // Load body info
    if (config["bodies"])
    {
        YAML::Node bodies_cfg = config["bodies"];
        for (int i = 0; i < bodies_cfg.size(); i++)
        {
            auto brc = new mushr_mujoco_ros::BodyROSConnector(&nh, bodies_cfg[i]);
            body_conn[brc->body_name()] = brc;
        }
    }

    ros::Publisher body_state_pub
        = nh.advertise<mushr_mujoco_ros::BodyStateArray>("body_state", 10);

    if (do_viz)
    {
        ROS_INFO("Starting visualization");
        viz::init();
    }

    rollout::init(&nh, &car_conn, &body_conn);

    SrvResponder srv_resp(car_conn, body_conn);
    ros::ServiceServer reset_srv
        = nh.advertiseService("reset", &SrvResponder::reset, &srv_resp);
    ros::ServiceServer step_srv
        = nh.advertiseService("step", &SrvResponder::step, &srv_resp);
    ros::ServiceServer get_state_srv
        = nh.advertiseService("state", &SrvResponder::get_state, &srv_resp);

    mjModel* m = mjglobal::mjmodel();
    mjData* d = NULL;

    while (ros::ok())
    {
        if (do_viz)
            viz::display();

        ros::spinOnce();
    }

    for (auto cc : car_conn)
        delete cc.second;
    for (auto bc : body_conn)
        delete bc.second;

    // free MuJoCo model and data, deactivate
    mjglobal::delete_model_and_data();
    mj_deactivate();

    viz::destroy();

// terminate GLFW (crashes with Linux NVidia drivers)
#if defined(__APPLE__) || defined(_WIN32)
    glfwTerminate();
#endif
}