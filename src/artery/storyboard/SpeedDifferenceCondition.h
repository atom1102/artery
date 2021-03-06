#ifndef SPEEDDIFFERENCECONDITION_H
#define SPEEDDIFFERENCECONDITION_H

#include "artery/storyboard/Condition.h"
#include "boost/units/quantity.hpp"
#include "boost/units/systems/si/velocity.hpp"

class SpeedDifferenceCondition : public Condition
{
public:
    SpeedDifferenceCondition(double difference) :
        mSpeedDifference(difference * boost::units::si::meter_per_second)
    {
    }

    virtual ConditionResult testCondition(const Vehicle& car) = 0;

protected:
    const boost::units::quantity<boost::units::si::velocity> mSpeedDifference;
};


class SpeedDifferenceConditionFaster : public SpeedDifferenceCondition
{
public:
    SpeedDifferenceConditionFaster(double difference) : SpeedDifferenceCondition(difference)
    {
    }

    virtual ConditionResult testCondition(const Vehicle& car) override;
};

class SpeedDifferenceConditionSlower : public SpeedDifferenceCondition
{
public:
    SpeedDifferenceConditionSlower(double difference) : SpeedDifferenceCondition(difference)
    {
    }

    virtual ConditionResult testCondition(const Vehicle& car) override;
};


#endif /* SPEEDDIFFERENCECONDITION_H */
