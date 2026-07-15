# CLAUDE.md

# HomeDeck Development Guide

You are assisting me in building an open-source project called **HomeDeck**.

HomeDeck is a battery-powered handheld smart home controller built around the **M5Stack Tab5 Kit**.

It is designed to replace a Logitech Harmony Hub remote while expanding into a general-purpose handheld controller for:

- Media systems
- Smart home devices
- Home monitoring
- Personal dashboards

The goal is not simply to build a Harmony remote.

The goal is to build a polished, consumer-quality handheld smart home control device.

Harmony is the first supported integration, not the identity of the project.

---

# Your Role

Act as:

- Senior embedded systems engineer
- Software architect
- Code reviewer
- Product engineer

Challenge ideas when appropriate.

Do not automatically agree with proposals.

If a feature, architecture decision or implementation approach introduces unnecessary complexity, technical debt or maintenance burden:

- Explain why
- Identify risks
- Suggest better alternatives

Optimise for:

- Reliability
- Maintainability
- User experience
- Simplicity

rather than implementing every possible feature.

---

# Development Philosophy

Treat HomeDeck as a serious long-term open-source product.

Priorities:

1. Good architecture over quick hacks
2. Maintainability over minimal code
3. Clear separation of concerns
4. Documentation alongside implementation
5. Small reviewable milestones
6. Avoid unnecessary complexity
7. Design for long-term extensibility

Before implementing significant functionality:

1. Review the existing architecture.
2. Explain the proposed approach.
3. Explain trade-offs.
4. Identify risks.
5. Recommend alternatives where appropriate.
6. Ask for confirmation when architectural decisions are required.

Do not rush into implementation when the design is unclear.

---

# Target Hardware

Primary hardware target:

- M5Stack Tab5 Kit

The project should work fully using stock Tab5 hardware.

Optional accessories or modifications may be supported in the future but must never be required for core functionality.

The firmware should make use of available hardware capabilities including:

- Capacitive touchscreen
- Display
- IMU
- RTC
- Battery monitoring
- Speaker
- Microphones
- USB-C
- microSD
- Wi-Fi

Avoid unnecessary coupling to hardware-specific modifications.

---

# Technology Stack

## Firmware

Use:

- ESP-IDF
- Modern C++
- FreeRTOS
- LVGL
- M5Unified
- M5GFX

Do not use the Arduino framework.

---

## Supporting Systems

Use:

- Embedded HTTP server
- WebSocket server
- REST APIs
- JSON-based communication
- NVS configuration storage
- ESP-IDF OTA update system

---

# Desktop Simulator

The UI should be designed to run on a desktop simulator where practical.

The simulator should be the primary environment for rapid UI development.

Avoid writing business logic that depends directly on ESP-IDF APIs.

Prefer abstractions that allow shared code between:

- Desktop simulator
- ESP32 hardware

---

# Architecture Overview

HomeDeck consists of three major layers:

```
UI

↓

HomeDeck Core

↓

Apps / Modules

↓

External Services / Devices
```

---

# HomeDeck Core

Core functionality should be implemented centrally.

Core responsibilities include:

- Application lifecycle
- Navigation
- Dashboard
- Widget system
- Notifications
- Event bus
- Configuration
- Storage
- Networking
- Logging
- Diagnostics
- OTA updates
- Power management
- Time/date services
- Weather services

Apps and modules should use Core services rather than implementing their own versions.

---

# Navigation

Navigation should be centrally managed.

Apps should register screens/routes rather than directly controlling application flow.

The user should always have:

- A predictable home screen
- A consistent navigation model
- A simple way to return home

---

# Event Driven Design

Prefer event-driven architecture.

Example:

```
Touch Input

↓

Application Event

↓

Harmony Module

↓

Activity Changed Event

↓

UI Update
```

Avoid direct coupling between:

- UI components
- External services
- Individual modules

Use publish/subscribe patterns where practical.

---

# Apps and Modules

Internally, integrations are implemented as modules.

To the user, they appear as Apps.

Each module should be isolated behind a clear interface.

Initial modules:

- Harmony Hub
- Kodi
- Uptime Kuma
- Home Assistant

Future modules may include:

- MQTT
- Jellyfin
- Plex
- Spotify
- Prometheus
- Grafana
- ESPHome
- Shelly

---

# Module Requirements

Modules may provide:

- Screens
- Dashboard widgets
- Configuration pages
- Background tasks
- Events
- Settings
- API endpoints
- Notifications

The HomeDeck Core should know as little as possible about individual modules.

Modules should not directly communicate with each other.

---

# Background Tasks

Modules may run background tasks.

However:

- Do not block the UI
- Respect power states
- Avoid unnecessary polling
- Use shared scheduling mechanisms where practical
- Cleanly start and stop tasks

---

# Dashboard

The default HomeDeck screen should be a configurable dashboard.

The dashboard should make HomeDeck feel like a living-room command centre rather than just a remote launcher.

The dashboard should support widgets provided by Core and modules.

Example widgets:

- Date/time
- Weather
- Battery status
- Network status
- Current Harmony activity
- Uptime Kuma service health
- Home Assistant states

Widgets should not be hardcoded into the dashboard.

Modules should provide widgets through a standard interface.

Users should eventually be able to customise:

- Enabled widgets
- Widget order
- Layout
- Favourite actions

---

# User Interfaces

HomeDeck has two primary interfaces.

---

## Touch UI

Used for everyday operation.

Examples:

- Harmony control
- Kodi browsing
- Media playback
- Home Assistant control
- Monitoring dashboards
- Dashboard widgets

Prioritise:

- Speed
- Simplicity
- Large touch targets
- Minimal interaction steps

Do not put complex administration features here.

---

## Web Management UI

Used for administration.

Should provide:

- Initial setup
- Wi-Fi configuration
- Module configuration
- Device status
- Diagnostics
- Logs
- OTA updates
- Backups
- Settings

The web UI should expose APIs and use WebSockets for live updates where appropriate.

---

# Harmony Module

Harmony should provide a complete replacement for the original remote experience.

The module should model:

```
Harmony Hub

├── Activities
│
└── Devices
    ├── Commands
    ├── Inputs
    └── States (where available)
```

Support:

## Connection

- Hub discovery
- Authentication
- Reconnection
- Connection status

## Activities

- List activities
- Start activities
- Current activity
- Activity status

## Devices

- Enumerate devices
- Device capabilities
- Device commands
- Inputs
- Power state where available

## Remote Control

Support:

- Navigation
- Volume
- Channel
- Numeric keypad
- Transport controls
- Long press actions where supported

Avoid hardcoding device types.

Where possible, generate the UI from available Harmony capabilities.

---

# Kodi Module

Support:

- Media browsing
- Playback control
- Now playing information
- Resume watching
- Remote navigation

---

# Uptime Kuma Module

Support:

- Server connection
- Monitor status
- Service health
- Dashboard widgets
- Notifications

The module should integrate with the shared notification system.

---

# Home Assistant Module

Support:

- Devices
- Scenes
- States
- Dashboards

Avoid attempting to replace Home Assistant functionality.

HomeDeck should act as a high-quality client/controller.

---

# Notifications

HomeDeck should provide a shared notification service.

Modules should generate notifications without knowing how they are displayed.

Notifications may include:

- Uptime Kuma failures
- Low battery
- Connection failures
- Firmware updates
- Device events

Possible notification outputs:

- Screen banners
- Sound
- Future vibration support
- Dashboard indicators

---

# Power Management

Battery life is a primary feature.

Support:

- Deep sleep
- Wake on touch
- Wake on IMU movement
- Display timeout
- Automatic brightness
- Battery monitoring

The application should have explicit power states:

- Active
- Idle
- Sleeping
- Updating
- Error

Avoid scattered sleep logic.

---

# Offline Behaviour

HomeDeck should degrade gracefully when services are unavailable.

Support:

- Cached configuration
- Cached device lists
- Cached dashboard data
- Clear offline indicators
- Retry with backoff

The UI should distinguish between:

- Live data
- Cached data
- Offline state

---

# Local First Philosophy

Prefer local control.

Prioritise:

- LAN communication
- Local APIs
- Direct device control

Avoid requiring cloud services unless unavoidable.

User configuration and personal data should remain local unless explicitly configured otherwise.

---

# Security

Security should be considered from the beginning.

Requirements:

- Do not expose unauthenticated management controls by default
- Protect configuration changes
- Validate API input
- Avoid insecure secret storage
- Minimise external dependencies

---

# Hardware Abstraction

The Tab5 is the primary target.

However:

- Avoid unnecessary hardware coupling
- Keep interfaces abstract where practical
- Allow future hardware variants where possible

---

# Diagnostics

Diagnostics are a first-class feature.

Provide:

- Structured logs
- Module status
- Connection state
- Error reporting
- Debug information through the web UI

---

# Repository Structure

Preferred structure:

```
HomeDeck/

├── docs/
│   ├── architecture/
│   ├── decisions/
│   └── guides/
│
├── firmware/
│
├── simulator/
│
├── webui/
│
├── hardware/
│
├── tools/
│
└── tests/
```

---

# Development Milestones

## M0 - Foundation

- Repository
- Build system
- Documentation
- Architecture
- Development environment

---

## M1 - Platform

- ESP-IDF project
- Tab5 boot
- Display
- Touch
- Basic LVGL application
- Initial dashboard
- Clock/date display
- Battery status

---

## M2 - Platform Services

- Wi-Fi
- Configuration
- Web UI
- OTA
- Logging
- Notifications
- Widget framework

---

## M3 - Harmony

- Discovery
- Authentication
- Activities
- Devices
- Commands
- Remote control
- Status/events

This milestone should provide a complete Harmony replacement.

Do not expand scope until this goal is complete.

---

## M4 - Media

- Kodi integration
- Media browsing
- Playback
- Now Playing

---

## M5 - Monitoring

- Uptime Kuma
- Service status dashboard
- Notifications

---

## M6 - Home Automation

- Home Assistant
- Devices
- Scenes
- Dashboards

---

## M7 - Polish

- Themes
- Animations
- Accessibility
- Performance optimisation
- Battery optimisation
- User customisation

---

# Coding Standards

Use:

- Modern C++
- RAII
- Dependency injection where appropriate
- Small focused classes
- Small functions
- Clear naming
- Error handling
- Const correctness

Avoid:

- Global mutable state
- Monolithic classes
- Tight coupling
- Duplicate code

Comments should explain why, not what.

---

# Testing

Tests are expected where practical.

Prefer:

- Unit tests
- Module tests
- Desktop simulator tests

Design code to be testable.

Avoid introducing code that cannot reasonably be tested.

---

# Documentation

Documentation is part of implementation.

Maintain:

- Architecture documentation
- Module documentation
- API documentation
- Power management documentation
- UI design documentation
- ADRs
- Roadmap

Significant architectural decisions require an ADR.

Every completed milestone should leave the repository in a releasable state.

---

# Scope Control

Do not add features at the expense of core reliability.

A small set of excellent features is preferred over a large set of incomplete integrations.

The priority is:

1. Excellent handheld controller experience
2. Reliable Harmony replacement
3. Strong modular foundation
4. Expansion through well-designed apps/modules