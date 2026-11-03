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

const unwrap = (v) => (v && typeof v === 'object' && 'value' in v ? v.value : v);

const num = (v) => {
    if (v === undefined) return undefined;
    const n = Number(unwrap(v));
    return Number.isFinite(n) ? n : undefined;
};

const onOff = (v) => {
    if (v === undefined) return undefined;
    const raw = unwrap(v);
    if (raw === undefined) return undefined;
    return raw ? 'ON' : 'OFF';
};

const zclTemp = (v) => {
    if (v === undefined) return undefined;
    const raw = unwrap(v);
    if (raw === 0x8000 || raw === -32768) return undefined;
    const t = Number(raw) / 100;
    return Number.isFinite(t) ? t : undefined;
};

const formatFirmware = (value) => {
    const version = Number(unwrap(value)) >>> 0;
    if (!Number.isFinite(Number(unwrap(value)))) return undefined;
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

        const power = onOff(data[ATTR.power]);
        const mode = num(data[ATTR.mode]);
        const fan = num(data[ATTR.fanMode]);
        const swing = num(data[ATTR.swingMode]);
        const preset = num(data[ATTR.preset]);
        const nanoex = onOff(data[ATTR.nanoex]);
        const eco = onOff(data[ATTR.eco]);
        const econavi = onOff(data[ATTR.econavi]);
        const mild = onOff(data[ATTR.mildDry]);
        const defrost = onOff(data[ATTR.defrost]);
        const indoor = num(data[ATTR.indoorTemp]);
        const outdoor = num(data[ATTR.outdoorTemp]);
        const target = num(data[ATTR.targetTemp]);
        const watts = num(data[ATTR.powerW]);
        const vswing = num(data[ATTR.vswing]);
        const hswing = num(data[ATTR.hswing]);
        const firmware = formatFirmware(data[ATTR.firmwareVersion]);

        if (power !== undefined) result.state = power;
        if (mode !== undefined) result.system_mode = ID_TO_MODE[mode] ?? 'auto';
        if (fan !== undefined) result.fan_mode = ID_TO_FAN[fan] ?? 'auto';
        if (swing !== undefined) result.swing_mode = ID_TO_SWING[swing] ?? 'off';
        if (preset !== undefined) result.preset = ID_TO_PRESET[preset] ?? 'none';
        if (nanoex !== undefined) result.nanoex = nanoex;
        if (eco !== undefined) result.eco = eco;
        if (econavi !== undefined) result.econavi = econavi;
        if (mild !== undefined) result.mild_dry = mild;
        if (defrost !== undefined) result.defrost = defrost;
        if (indoor !== undefined) result.local_temperature = indoor;
        if (outdoor !== undefined) result.outdoor_temperature = outdoor;
        if (target !== undefined) {
            result.occupied_heating_setpoint = target;
            result.occupied_cooling_setpoint = target;
        }
        if (watts !== undefined) result.power_consumption = watts;
        if (vswing !== undefined) result.vertical_swing = ID_TO_VSWING[vswing] ?? 'unknown';
        if (hswing !== undefined) result.horizontal_swing = ID_TO_HSWING[hswing] ?? 'unknown';
        if (firmware !== undefined) result.firmware_version = firmware;

        return result;
    },
};

const fromZigbeeThermostat = {
    cluster: 'hvacThermostat',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        const result = {};
        const local = zclTemp(msg.data.localTemp);
        const outdoor = zclTemp(msg.data.outdoorTemp);
        const cool = zclTemp(msg.data.occupiedCoolingSetpoint);
        const heat = zclTemp(msg.data.occupiedHeatingSetpoint);
        if (local !== undefined) result.local_temperature = local;
        if (outdoor !== undefined) result.outdoor_temperature = outdoor;
        if (cool !== undefined) result.occupied_cooling_setpoint = cool;
        if (heat !== undefined) result.occupied_heating_setpoint = heat;
        const systemMode = num(msg.data.systemMode);
        if (systemMode !== undefined) {
            const mode = zclSystemModeToMode[systemMode] ?? 'auto';
            result.system_mode = mode;
            result.state = mode === 'off' ? 'OFF' : 'ON';
        }
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
    system_mode: {
        key: ['system_mode'],
        convertSet: async (entity, _key, value) => {
            const name = String(value);
            if (!(name in MODE_TO_ID)) throw new Error(`Unsupported system_mode ${name}`);
            await writeAttr(entity, 'genAnalogInput', ATTR.mode, MODE_TO_ID[name], DATA_TYPE.uint8);
            return {state: {system_mode: name, state: name === 'off' ? 'OFF' : 'ON'}};
        },
        convertGet: async (entity) => readAttrs(entity, 'genAnalogInput', [ATTR.mode]),
    },
    local_temperature: {
        key: ['local_temperature'],
        convertSet: async () => {
            // Home Assistant publishes local_temperature as "" with climate commands.
            // The indoor temperature comes from the AC and is not writable.
        },
        convertGet: async (entity) => readAttrs(entity, 'genAnalogInput', [ATTR.indoorTemp]),
    },
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
    fingerprint: [{modelID: 'ZB-PANASONIC-AC', manufacturerName: 'ESP-PAC'}],
    model: 'ZB-PANASONIC-AC',
    vendor: 'ESP-PAC',
    description: 'Panasonic CZ-TACG1 (CN-CNT) air conditioner bridged by ESP32-C6 Zigbee',
    fromZigbee: [fromZigbeeAcAnalog, fromZigbeeThermostat],
    toZigbee: [
        toZigbee.state,
        toZigbee.system_mode,
        toZigbee.local_temperature,
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
        await readAttrs(endpoint, 'genAnalogInput', Object.values(ATTR));
        try {
            await endpoint.bind('hvacThermostat', coordinatorEndpoint);
            await endpoint.read('hvacThermostat', [
                'localTemp',
                'outdoorTemp',
                'occupiedCoolingSetpoint',
                'occupiedHeatingSetpoint',
                'systemMode',
            ]);
        } catch (error) {
            // Some coordinators reject the thermostat binding. Analog reports still carry the state.
        }
    },
};
