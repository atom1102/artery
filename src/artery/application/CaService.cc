#include "artery/application/CaService.h"
#include "artery/application/Asn1PacketVisitor.h"
#include "artery/application/VehicleDataProvider.h"
#include "artery/utility/simtime_cast.h"
#include "veins/base/utils/Coord.h"
#include <boost/units/cmath.hpp>
#include <boost/units/systems/si/prefixes.hpp>
#include <omnetpp/cexception.h>
#include <vanetza/btp/ports.hpp>
#include <chrono>

auto microdegree = vanetza::units::degree * boost::units::si::micro;
auto decidegree = vanetza::units::degree * boost::units::si::deci;
auto degree_per_second = vanetza::units::degree / vanetza::units::si::second;
auto centimeter_per_second = vanetza::units::si::meter_per_second * boost::units::si::centi;

static const simsignal_t scSignalCamReceived = cComponent::registerSignal("CaService.received");
static const simsignal_t scSignalCamSent = cComponent::registerSignal("CaService.sent");

Define_Module(CaService);


bool checkHeadingDelta(vanetza::units::Angle& prev, vanetza::units::Angle now)
{
	static const vanetza::units::Angle scHeadingDelta { 4.0 * vanetza::units::degree };
	return abs(prev - now) > scHeadingDelta;
}

bool checkPositionDelta(const Position& prev, const Position& now)
{
	static const vanetza::units::Length scPositionDelta { 4.0 * vanetza::units::si::meter };
	return (distance(prev, now) > scPositionDelta);
}

bool checkSpeedDelta(vanetza::units::Velocity prev, vanetza::units::Velocity now)
{
	static const vanetza::units::Velocity scSpeedDelta = 0.5 * vanetza::units::si::meter_per_second;
	return abs(prev - now) > scSpeedDelta;
}

static const auto scLowFrequencyContainerInterval = std::chrono::milliseconds(500);


CaService::CaService() :
		mVehicleDataProvider(nullptr),
		mTimer(nullptr),
		mGenCamMin { 100, SIMTIME_MS },
		mGenCamMax { 1000, SIMTIME_MS },
		mGenCam(mGenCamMax),
		mGenCamLowDynamicsCounter(0),
		mGenCamLowDynamicsLimit(3)
{
}

void CaService::initialize()
{
	ItsG5BaseService::initialize();
	mVehicleDataProvider = &getFacilities().get_const<VehicleDataProvider>();
	mTimer = &getFacilities().get_const<Timer>();
	// avoid unreasonable high elapsed time values for newly inserted vehicles
	mLastCamTimestamp = simTime();
	// first generated CAM shall include the low frequency container
	mLastLowCamTimestamp = mLastCamTimestamp - artery::simtime_cast(scLowFrequencyContainerInterval);
	mLocalDynamicMap = &getFacilities().get_mutable<artery::LocalDynamicMap>();
}

void CaService::trigger()
{
	checkTriggeringConditions(simTime());
}

void CaService::indicate(const vanetza::btp::DataIndication& ind, std::unique_ptr<vanetza::UpPacket> packet)
{
	Asn1PacketVisitor<vanetza::asn1::Cam> visitor;
	const vanetza::asn1::Cam* cam = boost::apply_visitor(visitor, *packet);
	if (cam) {
		// TODO: collect statistic data
		emit(scSignalCamReceived, cam->validate());
		mLocalDynamicMap->updateAwareness(*cam);
	}
}

void CaService::checkTriggeringConditions(const simtime_t& T_now)
{
	// provide variables named like in EN 302 637-2 V1.3.2 (section 6.1.3)
	simtime_t& T_GenCam = mGenCam;
	const simtime_t& T_GenCamMin = mGenCamMin;
	const simtime_t& T_GenCamMax = mGenCamMax;
	const simtime_t T_GenCamDcc = genCamDcc();
	const simtime_t T_elapsed = T_now - mLastCamTimestamp;

	if (T_elapsed >= T_GenCamDcc) {
		if (checkHeadingDelta(mLastCamHeading, mVehicleDataProvider->heading()) ||
			checkPositionDelta(mLastCamPosition, mVehicleDataProvider->position()) ||
			checkSpeedDelta(mLastCamSpeed, mVehicleDataProvider->speed())) {
			sendCam(T_now);
			T_GenCam = std::min(T_elapsed, T_GenCamMax); /*< if middleware update interval is too long */
			mGenCamLowDynamicsCounter = 0;
		} else if (T_elapsed >= T_GenCam) {
			sendCam(T_now);
			if (++mGenCamLowDynamicsCounter >= mGenCamLowDynamicsLimit) {
				T_GenCam = T_GenCamMax;
			}
		}
	}
}

void CaService::sendCam(const simtime_t& T_now)
{
	uint16_t genDeltaTimeMod = countTaiMilliseconds(mTimer->getTimeFor(mVehicleDataProvider->simtime()));
	auto cam = createCooperativeAwarenessMessage(*mVehicleDataProvider, genDeltaTimeMod);

	mLastCamPosition = mVehicleDataProvider->position();
	mLastCamSpeed = mVehicleDataProvider->speed();
	mLastCamHeading = mVehicleDataProvider->heading();
	mLastCamTimestamp = T_now;
	if (T_now - mLastLowCamTimestamp >= artery::simtime_cast(scLowFrequencyContainerInterval)) {
		addLowFrequencyContainer(cam);
		mLastLowCamTimestamp = T_now;
	}

	using namespace vanetza;
	btp::DataRequestB request;
	request.destination_port = btp::ports::CAM;
	request.gn.security_profile = security::Profile::CAM;
	request.gn.transport_type = geonet::TransportType::SHB;
	request.gn.traffic_class.tc_id(static_cast<unsigned>(dcc::Profile::DP2));
	request.gn.communication_profile = geonet::CommunicationProfile::ITS_G5;

	std::unique_ptr<geonet::DownPacket> payload { new geonet::DownPacket };
	payload->layer(OsiLayer::Application) = std::move(cam);
	const std::size_t payload_length = payload->size();
	this->request(request, std::move(payload));

	emit(scSignalCamSent, payload_length);
}

simtime_t CaService::genCamDcc()
{
	vanetza::Clock::duration delay = getFacilities().getDccScheduler().delay(vanetza::dcc::Profile::DP2);
	simtime_t dcc { std::chrono::duration_cast<std::chrono::milliseconds>(delay).count(), SIMTIME_MS };
	return std::min(mGenCamMax, std::max(mGenCamMin, dcc));
}

vanetza::asn1::Cam createCooperativeAwarenessMessage(const VehicleDataProvider& vdp, uint16_t genDeltaTime)
{
	vanetza::asn1::Cam message;

	ItsPduHeader_t& header = (*message).header;
	header.protocolVersion = ItsPduHeader__protocolVersion_currentVersion;
	header.messageID = ItsPduHeader__messageID_cam;
	header.stationID = vdp.station_id();

	CoopAwareness_t& cam = (*message).cam;
	cam.generationDeltaTime = genDeltaTime * GenerationDeltaTime_oneMilliSec;
	BasicContainer_t& basic = cam.camParameters.basicContainer;
	HighFrequencyContainer_t& hfc = cam.camParameters.highFrequencyContainer;

	basic.stationType = StationType_passengerCar;
	basic.referencePosition.altitude.altitudeValue = AltitudeValue_unavailable;
	basic.referencePosition.altitude.altitudeConfidence = AltitudeConfidence_unavailable;
	basic.referencePosition.longitude = (vdp.longitude() / microdegree).value() * Longitude_oneMicrodegreeEast;
	basic.referencePosition.latitude = (vdp.latitude() / microdegree).value() * Latitude_oneMicrodegreeNorth;
	basic.referencePosition.positionConfidenceEllipse.semiMajorOrientation = HeadingValue_unavailable;
	basic.referencePosition.positionConfidenceEllipse.semiMajorConfidence =
			SemiAxisLength_unavailable;
	basic.referencePosition.positionConfidenceEllipse.semiMinorConfidence =
			SemiAxisLength_unavailable;

	hfc.present = HighFrequencyContainer_PR_basicVehicleContainerHighFrequency;
	BasicVehicleContainerHighFrequency& bvc = hfc.choice.basicVehicleContainerHighFrequency;
	bvc.heading.headingValue = (vdp.heading() / decidegree).value();
	bvc.heading.headingConfidence = HeadingConfidence_equalOrWithinOneDegree;
	bvc.speed.speedValue = abs(vdp.speed() / centimeter_per_second).value() * SpeedValue_oneCentimeterPerSec;
	bvc.speed.speedConfidence = SpeedConfidence_equalOrWithinOneCentimeterPerSec * 3;
	bvc.driveDirection = vdp.speed().value() >= 0.0 ?
			DriveDirection_forward : DriveDirection_backward;
	const double lonAccelValue = vdp.acceleration() / vanetza::units::si::meter_per_second_squared;
	// extreme speed changes can occur when SUMO swaps vehicles between lanes (speed is swapped as well)
	if (lonAccelValue >= -160.0 && lonAccelValue <= 161.0) {
		bvc.longitudinalAcceleration.longitudinalAccelerationValue = lonAccelValue * LongitudinalAccelerationValue_pointOneMeterPerSecSquaredForward;
	} else {
		bvc.longitudinalAcceleration.longitudinalAccelerationValue = LongitudinalAccelerationValue_unavailable;
	}
	bvc.longitudinalAcceleration.longitudinalAccelerationConfidence = AccelerationConfidence_unavailable;
	bvc.curvature.curvatureValue = (vdp.curvature() / vanetza::units::reciprocal_metre) *
			CurvatureValue_reciprocalOf1MeterRadiusToLeft;
	bvc.curvature.curvatureConfidence = CurvatureConfidence_unavailable;
	bvc.curvatureCalculationMode = CurvatureCalculationMode_yawRateUsed;
	bvc.yawRate.yawRateValue = (vdp.yaw_rate() / degree_per_second).value() *
			YawRateValue_degSec_000_01ToLeft * 100.0;
	bvc.vehicleLength.vehicleLengthValue = VehicleLengthValue_unavailable;
	bvc.vehicleLength.vehicleLengthConfidenceIndication =
			VehicleLengthConfidenceIndication_noTrailerPresent;
	bvc.vehicleWidth = VehicleWidth_unavailable;

	std::string error;
	if (!message.validate(error)) {
		throw cRuntimeError("Invalid High Frequency CAM: %s", error.c_str());
	}

	return message;
}

void addLowFrequencyContainer(vanetza::asn1::Cam& message)
{
	LowFrequencyContainer_t*& lfc = message->cam.camParameters.lowFrequencyContainer;
	lfc = vanetza::asn1::allocate<LowFrequencyContainer_t>();
	lfc->present = LowFrequencyContainer_PR_basicVehicleContainerLowFrequency;
	BasicVehicleContainerLowFrequency& bvc = lfc->choice.basicVehicleContainerLowFrequency;
	bvc.vehicleRole = VehicleRole_default;
	bvc.exteriorLights.buf = static_cast<uint8_t*>(malloc(1));
	assert(nullptr != bvc.exteriorLights.buf);
	bvc.exteriorLights.size = 1;
	bvc.exteriorLights.buf[0] |= 1 << (7 - ExteriorLights_daytimeRunningLightsOn);
	// TODO: add pathHistory

	std::string error;
	if (!message.validate(error)) {
		throw cRuntimeError("Invalid Low Frequency CAM: %s", error.c_str());
	}
}
