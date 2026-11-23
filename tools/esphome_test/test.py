"""Drive the host build of the ESPHome API server with aioesphomeapi, the client Home Assistant uses.

Checks: handshake, device info, entity listing, state subscription, on/off and brightness commands, a change
made on the device side (SIGUSR1 = "web UI turned it off at 42 %") reaching the client, and a clean reconnect.
Run through run.sh.
"""
import asyncio
import os
import signal
import sys

from aioesphomeapi import APIClient, LightColorCapability, LightInfo, LightState

PORT = int(sys.argv[1])
SERVER_PID = int(sys.argv[2])


async def wait_for(states, pred, what, timeout=3.0):
    loop = asyncio.get_running_loop()
    end = loop.time() + timeout
    while loop.time() < end:
        if states and pred(states[-1]):
            return states[-1]
        await asyncio.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}; last state {states[-1] if states else None}")


async def main():
    cli = APIClient("127.0.0.1", PORT, "")
    await cli.connect(login=True)
    print("api version", cli.api_version)

    info = await cli.device_info()
    assert info.name == "hdmilight", info
    assert info.mac_address == "AA:BB:CC:DD:EE:FF", info
    assert info.webserver_port == 80, info
    assert info.manufacturer == "hdmilight" and info.model == "hdmilight-v3 (host)", info
    print("device info ok:", info.name, info.mac_address, info.model)

    entities, services = await cli.list_entities_services()
    lights = [e for e in entities if isinstance(e, LightInfo)]
    assert len(entities) == 1 and len(lights) == 1, entities
    light = lights[0]
    assert light.name == "" and light.object_id == "light", light
    modes = light.supported_color_modes_compat(cli.api_version)
    assert len(modes) == 1 and modes[0] & LightColorCapability.BRIGHTNESS, modes
    print("entity ok: key %08x, color modes %s" % (light.key, modes))

    states = []
    cli.subscribe_states(lambda s: states.append(s) if isinstance(s, LightState) else None)
    s = await wait_for(states, lambda s: s.key == light.key, "initial state")
    assert s.state is True and abs(s.brightness - 0.6) < 1e-6, s
    print("initial state ok:", s.state, round(s.brightness, 3))

    cli.light_command(light.key, state=False)
    await wait_for(states, lambda s: s.state is False, "off")
    print("off ok")

    cli.light_command(light.key, state=True, brightness=0.25)
    s = await wait_for(states, lambda s: s.state is True and abs(s.brightness - 0.25) < 1e-6, "on at 25 %")
    print("on + brightness ok:", round(s.brightness, 3))

    cli.light_command(light.key, brightness=0.9)
    await wait_for(states, lambda s: abs(s.brightness - 0.9) < 1e-6, "brightness 90 %")
    print("brightness-only ok")

    os.kill(SERVER_PID, signal.SIGUSR1)  # device-side change, as if from the web UI
    await wait_for(states, lambda s: s.state is False and abs(s.brightness - 0.42) < 1e-6, "device-side change")
    print("device-side change pushed ok")

    await cli.disconnect()

    cli2 = APIClient("127.0.0.1", PORT, "")
    await cli2.connect(login=True)
    info2 = await cli2.device_info()
    assert info2.name == "hdmilight"
    await cli2.disconnect()
    print("reconnect ok")
    print("ALL PASSED")


asyncio.run(main())
