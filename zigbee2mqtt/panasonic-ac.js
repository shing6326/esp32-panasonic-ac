const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;
const ea = exposes.access;

const ATTR = {
    power: 0xf000,
    mode: 0xf001,
    fanMode: 0xf002,
    swingMode: 0xf003,
    preset: 0xf004,
    nanoex: 0xf005,
    indoorTemp: 0xf006,
    outdoorTemp: 0xf007,
    targetTemp: 0xf008,
    firmwareVersion: 0xf009,
    eco: 0xf00a,
    econavi: 0xf00b,
    mildDry: 0xf00c,
    defrost: 0xf00d,
    powerW: 0xf00e,
    vswing: 0xf00f,
    hswing: 0xf010,
};

const DATA_TYPE = {
    bool: 0x10,
    uint8: 0x20,
    uint32: 0x23,
    single: 0x39,
};

const ID_TO_MODE = ['off', 'auto', 'cool', 'heat', 'dry', 'fan_only'];
const MODE_TO_ID = {off: 0, auto: 1, cool: 2, heat: 3, dry: 4, fan_only: 5};

const ID_TO_FAN = ['auto', '1', '2', '3', '4', '5'];
const FAN_TO_ID = {auto: 0, '1': 1, '2': 2, '3': 3, '4': 4, '5': 5};

const ID_TO_SWING = ['off', 'horizontal', 'vertical', 'both'];
const SWING_TO_ID = {off: 0, horizontal: 1, vertical: 2, both: 3};

const ID_TO_PRESET = ['none', 'powerful', 'quiet'];
const PRESET_TO_ID = {none: 0, powerful: 1, quiet: 2};

const ID_TO_VSWING = ['unknown', 'up', 'up_center', 'center', 'down_center', 'down', 'swing', 'auto'];
const VSWING_TO_ID = {
    unknown: 0, up: 1, up_center: 2, center: 3, down_center: 4, down: 5, swing: 6, auto: 7,
};

const ID_TO_HSWING = ['unknown', 'left', 'left_center', 'center', 'right_center', 'right', 'auto'];
const HSWING_TO_ID = {
    unknown: 0, left: 1, left_center: 2, center: 3, right_center: 4, right: 5, auto: 6,
};

const zclSystemModeToMode = {
    0: 'off',
    1: 'auto',
    3: 'cool',
    4: 'heat',
    7: 'fan_only',
    8: 'dry',
};

const formatFirmware = (value) => {
    const version = Number(value) >>> 0;
    return `${(version >>> 24) & 0xff}.${(version >>> 16) & 0xff}.${(version >>> 8) & 0xff}`;
};

const writeAttr = async (entity, cluster, attr, value, type) => {
    await entity.write(cluster, {[attr]: {value, type}});
};

const readAttrs = async (entity, cluster, attrs) => {
    await entity.read(cluster, attrs);
};

const fromZigbeeAcAnalog = {
    cluster: 'genAnalogInput',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const data = msg.data;
        const result = {};

        if (data[ATTR.power] !== undefined) result.state = data[ATTR.power] ? 'ON' : 'OFF';
        if (data[ATTR.mode] !== undefined) result.system_mode = ID_TO_MODE[data[ATTR.mode]] ?? 'auto';
        if (data[ATTR.fanMode] !== undefined) result.fan_mode = ID_TO_FAN[data[ATTR.fanMode]] ?? 'auto';
        if (data[ATTR.swingMode] !== undefined) result.swing_mode = ID_TO_SWING[data[ATTR.swingMode]] ?? 'off';
        if (data[ATTR.preset] !== undefined) result.preset = ID_TO_PRESET[data[ATTR.preset]] ?? 'none';
        if (data[ATTR.nanoex] !== undefined) result.nanoex = data[ATTR.nanoex] ? 'ON' : 'OFF';
        if (data[ATTR.eco] !== undefined) result.eco = data[ATTR.eco] ? 'ON' : 'OFF';
        if (data[ATTR.econavi] !== undefined) result.econavi = data[ATTR.econavi] ? 'ON' : 'OFF';
        if (data[ATTR.mildDry] !== undefined) result.mild_dry = data[ATTR.mildDry] ? 'ON' : 'OFF';
        if (data[ATTR.defrost] !== undefined) result.defrost = data[ATTR.defrost] ? 'ON' : 'OFF';
        if (data[ATTR.indoorTemp] !== undefined) result.local_temperature = data[ATTR.indoorTemp];
        if (data[ATTR.outdoorTemp] !== undefined) result.outdoor_temperature = data[ATTR.outdoorTemp];
        if (data[ATTR.targetTemp] !== undefined) {
            result.occupied_heating_setpoint = data[ATTR.targetTemp];
            result.occupied_cooling_setpoint = data[ATTR.targetTemp];
        }
        if (data[ATTR.powerW] !== undefined) result.power_consumption = data[ATTR.powerW];
        if (data[ATTR.vswing] !== undefined) result.vertical_swing = ID_TO_VSWING[data[ATTR.vswing]] ?? 'unknown';
        if (data[ATTR.hswing] !== undefined) result.horizontal_swing = ID_TO_HSWING[data[ATTR.hswing]] ?? 'unknown';
        if (data[ATTR.firmwareVersion] !== undefined) result.firmware_version = formatFirmware(data[ATTR.firmwareVersion]);

        return result;
    },
};

const fromZigbeeThermostat = {
    cluster: 'hvacThermostat',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const result = {};
        if (msg.data.localTemp !== undefined) result.local_temperature = msg.data.localTemp / 100;
        if (msg.data.outdoorTemp !== undefined) result.outdoor_temperature = msg.data.outdoorTemp / 100;
        if (msg.data.occupiedCoolingSetpoint !== undefined) {
            result.occupied_heating_setpoint = msg.data.occupiedCoolingSetpoint / 100;
        }
        if (msg.data.occupiedHeatingSetpoint !== undefined) {
            result.occupied_heating_setpoint = msg.data.occupiedHeatingSetpoint / 100;
        }
        if (msg.data.systemMode !== undefined) result.system_mode = zclSystemModeToMode[msg.data.systemMode] ?? 'auto';
        return result;
    },
};

const enumSet = (key, table, attr) => ({
    key: [key],
    convertSet: async (entity, _key, value) => {
        const name = String(value);
        if (!(name in table)) throw new Error(`Unsupported ${key} ${name}`);
        await writeAttr(entity, 'genAnalogInput', attr, table[name], DATA_TYPE.uint8);
        return {state: {[key]: name}};
    },
    convertGet: async (entity) => readAttrs(entity, 'genAnalogInput', [attr]),
});

const binarySet = (key, attr) => ({
    key: [key],
    convertSet: async (entity, _key, value) => {
        const on = value === 'ON' || value === true;
        await writeAttr(entity, 'genAnalogInput', attr, on, DATA_TYPE.bool);
        return {state: {[key]: on ? 'ON' : 'OFF'}};
    },
    convertGet: async (entity) => readAttrs(entity, 'genAnalogInput', [attr]),
});

const toZigbee = {
    state: {
        key: ['state'],
        convertSet: async (entity, _key, value) => {
            const on = value === 'ON' || value === true;
            await writeAttr(entity, 'genAnalogInput', ATTR.power, on, DATA_TYPE.bool);
            return {state: {state: on ? 'ON' : 'OFF'}};
        },
    },
    system_mode: enumSet('system_mode', MODE_TO_ID, ATTR.mode),
    occupied_cooling_setpoint: {
        key: ['occupied_cooling_setpoint', 'occupied_heating_setpoint', 'current_heating_setpoint', 'current_cooling_setpoint'],
        convertSet: async (entity, _key, value) => {
            const temp = Math.round(Number(value) * 2) / 2;
            if (!Number.isFinite(temp) || temp < 16 || temp > 30) throw new Error('Temperature must be 16..30');
            await writeAttr(entity, 'genAnalogInput', ATTR.targetTemp, temp, DATA_TYPE.single);
            return {state: {occupied_heating_setpoint: temp, occupied_cooling_setpoint: temp, state: 'ON'}};
        },
        convertGet: async (entity) => readAttrs(entity, 'genAnalogInput', [ATTR.targetTemp]),
    },
    fan_mode: enumSet('fan_mode', FAN_TO_ID, ATTR.fanMode),
    swing_mode: enumSet('swing_mode', SWING_TO_ID, ATTR.swingMode),
    preset: enumSet('preset', PRESET_TO_ID, ATTR.preset),
    vertical_swing: enumSet('vertical_swing', VSWING_TO_ID, ATTR.vswing),
    horizontal_swing: enumSet('horizontal_swing', HSWING_TO_ID, ATTR.hswing),
    nanoex: binarySet('nanoex', ATTR.nanoex),
    eco: binarySet('eco', ATTR.eco),
    econavi: binarySet('econavi', ATTR.econavi),
    mild_dry: binarySet('mild_dry', ATTR.mildDry),
};

module.exports = {
    fingerprint: [{modelID: 'ZB-PANASONIC-AC', manufacturerName: 'Panasonic'}],
    model: 'ZB-PANASONIC-AC',
    vendor: 'Panasonic',
    description: 'Panasonic CZ-TACG1 (CN-CNT) air conditioner bridged by ESP32-C6/H2 Zigbee',
    fromZigbee: [fromZigbeeAcAnalog, fromZigbeeThermostat],
    toZigbee: [
        toZigbee.state,
        toZigbee.system_mode,
        toZigbee.occupied_cooling_setpoint,
        toZigbee.fan_mode,
        toZigbee.swing_mode,
        toZigbee.preset,
        toZigbee.vertical_swing,
        toZigbee.horizontal_swing,
        toZigbee.nanoex,
        toZigbee.eco,
        toZigbee.econavi,
        toZigbee.mild_dry,
    ],
    exposes: [
        e.climate()
            .withSetpoint('occupied_heating_setpoint', 16, 30, 0.5, ea.STATE_SET)
            .withLocalTemperature()
            .withSystemMode(['off', 'auto', 'cool', 'heat', 'dry', 'fan_only'], ea.STATE_SET),
        e.enum('fan_mode', ea.STATE_SET, ID_TO_FAN).withDescription('Fan speed'),
        e.enum('swing_mode', ea.STATE_SET, ID_TO_SWING).withDescription('Swing mode'),
        e.enum('preset', ea.STATE_SET, ID_TO_PRESET).withDescription('Powerful or quiet'),
        e.enum('vertical_swing', ea.STATE_SET, ID_TO_VSWING).withDescription('Vertical vane position'),
        e.enum('horizontal_swing', ea.STATE_SET, ID_TO_HSWING).withDescription('Horizontal vane position'),
        e.binary('nanoex', ea.STATE_SET, 'ON', 'OFF').withDescription('nanoeX. Only enable if the remote has this button'),
        e.binary('eco', ea.STATE_SET, 'ON', 'OFF').withDescription('Eco. Only enable if the remote has this button'),
        e.binary('econavi', ea.STATE_SET, 'ON', 'OFF').withDescription('ECONAVI. Only enable if the remote has this button'),
        e.binary('mild_dry', ea.STATE_SET, 'ON', 'OFF').withDescription('Mild dry. Only enable if the remote has this button'),
        e.binary('defrost', ea.STATE, 'ON', 'OFF').withDescription('Outdoor unit defrost'),
        e.numeric('outdoor_temperature', ea.STATE).withUnit('°C').withDescription('Outdoor temperature'),
        e.numeric('power_consumption', ea.STATE).withUnit('W').withDescription('Estimated power reported by the AC'),
        e.text('firmware_version', ea.STATE).withDescription('Firmware version'),
    ],
    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(1);
        await endpoint.bind('genAnalogInput', coordinatorEndpoint);
        try {
            await endpoint.bind('hvacThermostat', coordinatorEndpoint);
        } catch (error) {
            // Some coordinators reject the thermostat binding. Analog reports still carry the state.
        }
    },
};
