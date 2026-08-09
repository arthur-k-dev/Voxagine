#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "Core/Math.h"

/* A check: one small, deterministic assertion about one unit of the engine.
 *
 * This is the level below Scenario.h. A scenario runs the whole destruction
 * pipeline over a world and asks whether the invariants held; a check calls one
 * function with one input and says what it must return. Both are needed and
 * neither substitutes for the other - the play-session defects were missing
 * *scenarios*, while the chunk-index off-by-ones were caught by *checks*.
 *
 * Checks live under Tests/<System>/<Thing>Checks.cpp, where <System> names the
 * part of the engine under test rather than the header's directory. A file's
 * path should answer "what does this test?" without opening it.
 *
 * These macros used to be Google Test's. They are ours now for one reason: this
 * tree should have one test system rather than three, and gtest cannot register
 * a scenario or a benchmark. The spelling is deliberately close so that a body
 * written against gtest still reads correctly - CHECK_ is a non-fatal
 * expectation, REQUIRE_ abandons the case.
 */
class CheckContext
{
public:
	void Fail(const char* pFile, int iLine, const std::string& detail);

	/* REQUIRE_ sets this and returns; the runner reports the case as failed
	   without running the rest of its body. */
	void Abandon() { m_bAbandoned = true; }
	bool Abandoned() const { return m_bAbandoned; }

	bool Failed() const { return !m_Failures.empty(); }
	const std::vector<std::string>& Failures() const { return m_Failures; }

private:
	std::vector<std::string> m_Failures;
	bool m_bAbandoned = false;
};

class Check
{
public:
	virtual ~Check() = default;

	/* The system under test, which is also the directory the file lives in. */
	virtual const char* System() const = 0;
	virtual const char* Name() const = 0;

	virtual void Run(CheckContext& ctx) const = 0;
};

class CheckRegistry
{
public:
	static CheckRegistry& Get();

	bool Add(std::unique_ptr<Check> pCheck);

	const std::vector<std::unique_ptr<Check>>& Checks() const { return m_Checks; }

private:
	std::vector<std::unique_ptr<Check>> m_Checks;
};

/* Value formatting for failure messages.
 *
 * Anything streamable prints itself; anything else prints a placeholder rather
 * than failing to compile, because a check comparing a type with no operator<<
 * is perfectly reasonable and the expression text still names it. */
namespace CheckDetail
{
	template <typename T, typename = void>
	struct IsStreamable : std::false_type {};

	template <typename T>
	struct IsStreamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
		: std::true_type {};

	template <typename T>
	std::enable_if_t<IsStreamable<T>::value, std::string> Format(const T& value)
	{
		std::ostringstream stream;
		stream << value;

		return stream.str();
	}

	template <typename T>
	std::enable_if_t<!IsStreamable<T>::value, std::string> Format(const T&)
	{
		return "<value>";
	}

	inline std::string Format(bool bValue) { return bValue ? "true" : "false"; }

	inline std::string Format(const Vector3& v3Value)
	{
		std::ostringstream stream;
		stream << "(" << v3Value.x << ", " << v3Value.y << ", " << v3Value.z << ")";

		return stream.str();
	}

	inline std::string Format(const UVector3& v3Value)
	{
		std::ostringstream stream;
		stream << "(" << v3Value.x << ", " << v3Value.y << ", " << v3Value.z << ")";

		return stream.str();
	}

	/* Comparison in a function template rather than in the macro, so `a` and `b`
	   are evaluated exactly once each - a check calling a counter twice would
	   otherwise measure the wrong thing. */
	template <typename A, typename B>
	bool Equal(const A& a, const B& b) { return a == b; }

	template <typename A, typename B>
	bool Near(const A& a, const B& b, double fTolerance)
	{
		const double fDelta = static_cast<double>(a) - static_cast<double>(b);

		return (fDelta < 0.0 ? -fDelta : fDelta) <= fTolerance;
	}

	/* What a failing CHECK_ leaves behind, and what makes
	   `CHECK_TRUE(x) << "at " << i;` work.
	 *
	 * A passing check builds one of these holding no context, streams into
	 * nothing and records nothing on destruction - which matters, because the
	 * checks that most want a trailing message are the ones inside a triple
	 * loop over a voxel window, and formatting a message per passing assertion
	 * would cost more than the check. */
	class Recorder
	{
	public:
		Recorder() = default;

		Recorder(CheckContext& ctx, const char* pFile, int iLine, std::string detail) :
			m_pContext(&ctx), m_pFile(pFile), m_iLine(iLine), m_Detail(std::move(detail))
		{
		}

		~Recorder()
		{
			if (m_pContext != nullptr)
				m_pContext->Fail(m_pFile, m_iLine, m_Detail);
		}

		Recorder(const Recorder&) = delete;
		Recorder& operator=(const Recorder&) = delete;

		template <typename T>
		Recorder& operator<<(const T& value)
		{
			if (m_pContext != nullptr)
			{
				std::ostringstream stream;
				stream << value;

				m_Detail += m_bStreamed ? "" : "  ";
				m_Detail += stream.str();

				m_bStreamed = true;
			}

			return *this;
		}

	private:
		CheckContext* m_pContext = nullptr;
		const char* m_pFile = "";
		int m_iLine = 0;

		std::string m_Detail;
		bool m_bStreamed = false;
	};

	/* The same trailing-message support for the fatal forms, which cannot use
	   Recorder: recording happens in a destructor there, and a destructor
	   cannot return out of the enclosing case.
	 *
	 * So the message is streamed into a Message first and handed to a sink
	 * whose operator= returns void, letting the macro end in
	 * `return sink = Message() << ...;`. That is Google Test's shape and it is
	 * the only one that gets both a trailing message and an early return out of
	 * a single expression. */
	class Message
	{
	public:
		template <typename T>
		Message& operator<<(const T& value)
		{
			m_Stream << (m_bAny ? " " : "") << value;
			m_bAny = true;

			return *this;
		}

		std::string Str() const { return m_bAny ? "  " + m_Stream.str() : std::string(); }

	private:
		std::ostringstream m_Stream;
		bool m_bAny = false;
	};

	class AbandonHelper
	{
	public:
		AbandonHelper(CheckContext& ctx, const char* pFile, int iLine, std::string detail) :
			m_Context(ctx), m_pFile(pFile), m_iLine(iLine), m_Detail(std::move(detail))
		{
		}

		void operator=(const Message& message) const
		{
			m_Context.Fail(m_pFile, m_iLine, m_Detail + message.Str());
			m_Context.Abandon();
		}

	private:
		CheckContext& m_Context;
		const char* m_pFile;
		int m_iLine;
		std::string m_Detail;
	};
}

/* The non-fatal forms yield a Recorder, so `CHECK_EQ(a, b) << "at " << i;` is a
   valid statement and the trailing message reaches the report. Wrapped in a
   lambda because the operands have to be named to be formatted and must still
   be evaluated exactly once. */
#define VOXAGINE_CHECK_BINARY(a, b, op, description)                                               \
	[&](const auto& voxCheckA, const auto& voxCheckB) -> CheckDetail::Recorder                     \
	{                                                                                              \
		if (op)                                                                                    \
			return CheckDetail::Recorder();                                                        \
                                                                                                   \
		return CheckDetail::Recorder(ctx, __FILE__, __LINE__,                                      \
			std::string(#a " " description " " #b "  (") +                                         \
			CheckDetail::Format(voxCheckA) + " vs " + CheckDetail::Format(voxCheckB) + ")");       \
	}((a), (b))

#define VOXAGINE_CHECK_UNARY(expr, bExpected)                                                      \
	[&]() -> CheckDetail::Recorder                                                                 \
	{                                                                                              \
		if (static_cast<bool>(expr) == (bExpected))                                                \
			return CheckDetail::Recorder();                                                        \
                                                                                                   \
		return CheckDetail::Recorder(ctx, __FILE__, __LINE__,                                      \
			std::string(#expr " is ") + ((bExpected) ? "false" : "true"));                         \
	}()

#define CHECK_EQ(a, b)   VOXAGINE_CHECK_BINARY(a, b, CheckDetail::Equal(voxCheckA, voxCheckB), "==")
#define CHECK_NE(a, b)   VOXAGINE_CHECK_BINARY(a, b, !CheckDetail::Equal(voxCheckA, voxCheckB), "!=")
#define CHECK_LT(a, b)   VOXAGINE_CHECK_BINARY(a, b, voxCheckA <  voxCheckB, "<")
#define CHECK_LE(a, b)   VOXAGINE_CHECK_BINARY(a, b, voxCheckA <= voxCheckB, "<=")
#define CHECK_GT(a, b)   VOXAGINE_CHECK_BINARY(a, b, voxCheckA >  voxCheckB, ">")
#define CHECK_GE(a, b)   VOXAGINE_CHECK_BINARY(a, b, voxCheckA >= voxCheckB, ">=")
#define CHECK_TRUE(expr)  VOXAGINE_CHECK_UNARY(expr, true)
#define CHECK_FALSE(expr) VOXAGINE_CHECK_UNARY(expr, false)

#define CHECK_NEAR(a, b, tolerance) \
	VOXAGINE_CHECK_BINARY(a, b, CheckDetail::Near(voxCheckA, voxCheckB, (tolerance)), "~=")

/* The fatal forms abandon the case: they record, then return out of the check
   body, so nothing after them runs.
 *
 * Use these only where continuing would be nonsense rather than merely noisy: a
 * null pointer about to be dereferenced, a container about to be indexed.
 * Everything else should be CHECK_, so one run reports every disagreement
 * instead of only the first.
 *
 * The `switch (0) case 0: default:` is not decoration. It makes the macro a
 * single statement that still swallows a dangling `else`, and it lets the
 * expression end in `return sink = Message() << ...`, which is what gets both a
 * trailing message and an early return out of one expression. */
#define VOXAGINE_REQUIRE(condition, detail)                                                        \
	switch (0) case 0: default:                                                                    \
		if (condition) ;                                                                           \
		else return CheckDetail::AbandonHelper(ctx, __FILE__, __LINE__, (detail)) = CheckDetail::Message()

#define REQUIRE_EQ(a, b) \
	VOXAGINE_REQUIRE(CheckDetail::Equal((a), (b)), \
		std::string(#a " == " #b "  (") + CheckDetail::Format(a) + " vs " + CheckDetail::Format(b) + ")")

#define REQUIRE_NE(a, b) \
	VOXAGINE_REQUIRE(!CheckDetail::Equal((a), (b)), \
		std::string(#a " != " #b "  (") + CheckDetail::Format(a) + " vs " + CheckDetail::Format(b) + ")")

#define REQUIRE_GT(a, b) \
	VOXAGINE_REQUIRE((a) > (b), \
		std::string(#a " > " #b "  (") + CheckDetail::Format(a) + " vs " + CheckDetail::Format(b) + ")")

#define REQUIRE_TRUE(expr)  VOXAGINE_REQUIRE((expr),  std::string(#expr " is false"))
#define REQUIRE_FALSE(expr) VOXAGINE_REQUIRE(!(expr), std::string(#expr " is true"))

/* Declares and registers one check. The class name is unique rather than
   anonymous because the body is defined out of line, which an anonymous
   namespace cannot span. Same shape as the scenario and benchmark macros. */
#define VOXAGINE_CHECK(SystemName, CaseName)                                                       \
	struct VoxCheck_##SystemName##_##CaseName final : public Check                                 \
	{                                                                                              \
		const char* System() const override { return #SystemName; }                                \
		const char* Name() const override { return #CaseName; }                                    \
		void Run(CheckContext& ctx) const override;                                                \
	};                                                                                             \
                                                                                                   \
	static const bool s_bVoxCheck_##SystemName##_##CaseName =                                      \
		CheckRegistry::Get().Add(std::unique_ptr<Check>(new VoxCheck_##SystemName##_##CaseName())); \
                                                                                                   \
	void VoxCheck_##SystemName##_##CaseName::Run(CheckContext& ctx) const
