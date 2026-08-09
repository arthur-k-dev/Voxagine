#pragma once

class ImPlatform {
public:
	virtual ~ImPlatform() = default;

	virtual void Initialize() = 0;
	virtual void NewFrame() = 0;
};