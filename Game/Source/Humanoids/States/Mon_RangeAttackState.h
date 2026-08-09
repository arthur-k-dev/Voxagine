#pragma once
#include "AI/States/FSMState.h"
#include "Humanoids/Enemies/Monster.h"

class Mon_RangeAttackState : public FSMState<Monster>
{
private:
	float m_fTimer = 0.f;
	bool m_bHasShot = false;

public:
	void Start(Monster* pOwner) override;
	void Tick(Monster* pOwner, float fDeltaTime) override;
	void Exit(Monster* pOwner) override;
};