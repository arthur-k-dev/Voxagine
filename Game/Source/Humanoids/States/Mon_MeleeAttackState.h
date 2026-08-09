#pragma once
#include "AI/States/FSMState.h"
#include "Humanoids/Enemies/Monster.h"

class Mon_MeleeAttackState : public FSMState<Monster>
{
private:
	float m_fTimer = 0.f;
	Vector3 m_velocity = Vector3(0.f);
	bool m_bShouldSeek = false;

public:
	void Start(Monster* pOwner) override;
	void Tick(Monster* pOwner, float fDeltaTime) override;
	void Exit(Monster* pOwner) override;
};