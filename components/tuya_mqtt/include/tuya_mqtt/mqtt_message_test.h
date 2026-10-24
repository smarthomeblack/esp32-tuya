#pragma once

namespace tuya_mqtt {

/// Golden-sample self-test for the MQTT message path (parsing, topics,
/// reassembly); gates network startup on the result.
bool RunMqttMessageSelfTest();

}  // namespace tuya_mqtt
