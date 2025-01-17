// Copyright (c) 2023 Oleg Kalachev <okalachev@gmail.com>
// Repository: https://github.com/okalachev/flix

// Gazebo plugin for running Arduino code and simulating the drone

#include <functional>
#include <cmath>
#include <gazebo/gazebo.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo/common/common.hh>
#include <gazebo/sensors/sensors.hh>
#include <gazebo/msgs/msgs.hh>
#include <ignition/math/Vector3.hh>
#include <ignition/math/Pose3.hh>
#include <iostream>
#include <fstream>

#include "Arduino.h"
#include "flix.h"
#include "util.h"
#include "util.ino"
#include "rc.ino"
#include "time.ino"
#include "estimate.ino"
#include "control.ino"
#include "log.ino"
#include "cli.ino"
#include "lpf.h"

using ignition::math::Vector3d;
using namespace gazebo;
using namespace std;

class ModelFlix : public ModelPlugin {
private:
	physics::ModelPtr model;
	physics::LinkPtr body;
	sensors::ImuSensorPtr imu;
	event::ConnectionPtr updateConnection, resetConnection;
	transport::NodePtr nodeHandle;
	transport::PublisherPtr motorPub[4];
	LowPassFilter<Vector> accFilter = LowPassFilter<Vector>(0.1);

public:
	void Load(physics::ModelPtr _parent, sdf::ElementPtr /*_sdf*/) {
		this->model = _parent;
		this->body = this->model->GetLink("body");
		this->imu = dynamic_pointer_cast<sensors::ImuSensor>(sensors::get_sensor(model->GetScopedName(true) + "::body::imu")); // default::flix::body::imu
		this->updateConnection = event::Events::ConnectWorldUpdateBegin(bind(&ModelFlix::OnUpdate, this));
		this->resetConnection = event::Events::ConnectWorldReset(bind(&ModelFlix::OnReset, this));
		initNode();
		Serial.begin(0);
		gzmsg << "Flix plugin loaded" << endl;
	}

	void OnReset() {
		attitude = Quaternion(); // reset estimated attitude
		gzmsg << "Flix plugin reset" << endl;
	}

	void OnUpdate() {
		__micros = model->GetWorld()->SimTime().Double() * 1000000;
		step();

		// read imu
		rates = flu2frd(imu->AngularVelocity());
		acc = this->accFilter.update(flu2frd(imu->LinearAcceleration()));

		// read rc
		readRC();
		controls[RC_CHANNEL_MODE] = 1; // 0 acro, 1 stab
		controls[RC_CHANNEL_ARMED] = 1; // armed

		estimate();

		// correct yaw to the actual yaw
		attitude.setYaw(-this->model->WorldPose().Yaw());

		control();
		parseInput();

		applyMotorForces();
		publishTopics();
		logData();
	}

	void applyMotorForces() {
		// thrusts
		const double dist = 0.035355; // motors shift from the center, m
		const double maxThrust = 0.03 * ONE_G; // ~30 g, https://youtu.be/VtKI4Pjx8Sk?&t=78

		const float scale0 = 1.0, scale1 = 1.1, scale2 = 0.9, scale3 = 1.05; // imitating motors asymmetry
		float mfl = scale0 * maxThrust * abs(motors[MOTOR_FRONT_LEFT]);
		float mfr = scale1 * maxThrust * abs(motors[MOTOR_FRONT_RIGHT]);
		float mrl = scale2 * maxThrust * abs(motors[MOTOR_REAR_LEFT]);
		float mrr = scale3 * maxThrust * abs(motors[MOTOR_REAR_RIGHT]);

		body->AddLinkForce(Vector3d(0.0, 0.0, mfl), Vector3d(dist, dist, 0.0));
		body->AddLinkForce(Vector3d(0.0, 0.0, mfr), Vector3d(dist, -dist, 0.0));
		body->AddLinkForce(Vector3d(0.0, 0.0, mrl), Vector3d(-dist, dist, 0.0));
		body->AddLinkForce(Vector3d(0.0, 0.0, mrr), Vector3d(-dist, -dist, 0.0));

		// torque
		const double maxTorque = 0.0024; // 24 g*cm ≈ 24 mN*m
		body->AddRelativeTorque(Vector3d(0.0, 0.0, scale0 * maxTorque * motors[MOTOR_FRONT_LEFT]));
		body->AddRelativeTorque(Vector3d(0.0, 0.0, scale1 * -maxTorque * motors[MOTOR_FRONT_RIGHT]));
		body->AddRelativeTorque(Vector3d(0.0, 0.0, scale2 * -maxTorque * motors[MOTOR_REAR_LEFT]));
		body->AddRelativeTorque(Vector3d(0.0, 0.0, scale3 * maxTorque * motors[MOTOR_REAR_RIGHT]));
	}

	void initNode() {
		nodeHandle = transport::NodePtr(new transport::Node());
		nodeHandle->Init();
		string ns = "~/" + model->GetName();
		// create motors output topics for debugging and plotting
		motorPub[0] = nodeHandle->Advertise<msgs::Int>(ns + "/motor0");
		motorPub[1] = nodeHandle->Advertise<msgs::Int>(ns + "/motor1");
		motorPub[2] = nodeHandle->Advertise<msgs::Int>(ns + "/motor2");
		motorPub[3] = nodeHandle->Advertise<msgs::Int>(ns + "/motor3");
	}

	void publishTopics() {
		for (int i = 0; i < 4; i++) {
			msgs::Int msg;
			msg.set_data(static_cast<int>(round(motors[i] * 1000)));
			motorPub[i]->Publish(msg);
		}
	}
};

GZ_REGISTER_MODEL_PLUGIN(ModelFlix)
