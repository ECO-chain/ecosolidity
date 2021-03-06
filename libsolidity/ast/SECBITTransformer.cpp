/*
    The SECBIT Solidity Safe Subsect.

    Copyright (c) SECBIT Labs 2018.

    This file is part of solidity.

    solidity is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    solidity is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef SECBIT

#include <libsolidity/ast/SECBITTransformer.h>
#include <libsolidity/ast/AST.h>

#include <boost/algorithm/string.hpp>

using namespace std;

namespace dev
{
namespace solidity
{

SECBITTransformer::SECBITTransformer(
	ASTNode const& _ast,
	string const& _source
): m_source(_source), m_ast(&_ast)
{
}

void SECBITTransformer::getReplacement(
	SourceLocation const& _loc,
	ostream& _stream)
{
	auto i = begin(m_replacements);
	int cur = _loc.start;
	while(i != end(m_replacements)) {
		if(_loc.contains(i->first)) {
			_stream << m_source.substr(cur, i->first.start - cur);
			_stream << i->second;
			cur = i->first.end;
			m_replacements.erase(i++);
		} else {
			i++;
		}
	}
	_stream << m_source.substr(cur, _loc.end - cur);
}

static inline bool isSafeMath(string const& _str)
{
	return boost::algorithm::to_lower_copy(_str) == "safemath";
}

void SECBITTransformer::print(ostream& _stream)
{
	// Skip if we already have SafeMath.
	if(auto const* src = asC<SourceUnit>(m_ast)) {
		for(auto& p : src->nodes()) {
			if(auto const* c = asC<ContractDefinition>(p.get())) {
				if(isSafeMath(c->name())) {
					return;
				}
			}
		}
	}

	m_ast->accept(*this);

	SourceLocation all(m_ast->location());
	all.end = m_source.length();
	getReplacement(all, _stream);
}

bool SECBITTransformer::visit(ContractDefinition const&)
{
	m_needReplace = true;
	m_hasReplace = false;
	return true;
}

// SafeMath.
// https://github.com/OpenZeppelin/openzeppelin-solidity/blob/master/contracts/math/SafeMath.sol
// @commit 9aa30e19605e686d324448645c6594d2d31464c6
const char *const safeMathSource = R"blahblah(

// SafeMath library from openzeppelin.
library SafeMath {
  function mul(uint256 _a, uint256 _b) internal pure returns (uint256) {
    if (_a == 0) {
      return 0;
    }
    uint256 c = _a * _b;
    require(c / _a == _b);
    return c;
  }

  function sub(uint256 _a, uint256 _b) internal pure returns (uint256) {
    require(_b <= _a);
    uint256 c = _a - _b;
    return c;
  }

  function add(uint256 _a, uint256 _b) internal pure returns (uint256) {
    uint256 c = _a + _b;
    require(c >= _a);
    return c;
  }
}

)blahblah";

// Using for directive.
const char *const usingSource = R"blahblah(using SafeMath for uint256;
    )blahblah";

void SECBITTransformer::endVisit(ContractDefinition const& _node)
{
	if(!m_hasReplace) {
		return;
	}

	auto injectBefore = [&](ASTNode const* _node, string const& str) {
		SourceLocation before(_node->location());
		before.end = before.start;
		m_replacements.emplace(make_pair(move(before), str));
	};

	// Inject SafeMath library before the first contract using it.
	if(m_firstContract) {
		m_firstContract = false;
		injectBefore(&_node, safeMathSource);
	}

	// Inject using directive before the first subnode.
	injectBefore(_node.subNodes().front().get(), usingSource);
	return;
}

bool SECBITTransformer::visit(UsingForDirective const& _node)
{
	// Skip if already using SafeMath.
	if(isSafeMath(_node.libraryName().namePath().back())) {
		m_needReplace = false;
	}
	return false;
}

// Return true if this type is uin256.
static bool isUint256(Type const* _type)
{
	if(auto const* intType = asC<IntegerType>(_type)) {
		if(intType->numBits() == 256
		   && !intType->isAddress()
		   && !intType->isSigned()) {
			return true;
		}
	}
	return false;
}

const char *const subStr = "sub";
const char *const mulStr = "mul";
const char *const addStr = "add";

// Get the string for the overflow operator.
// Return null if _token is not an overflow operator.
static char const* getOverflowOperator(
	Token::Value const& _token,
	Type const* _type)
{
	char const* rv;

	switch(_token) {
	case Token::AssignAdd:
	case Token::Add:
		rv = addStr;
		break;
	case Token::AssignSub:
	case Token::Sub:
	        rv = subStr;
		break;
	case Token::AssignMul:
	case Token::Mul:
		rv = mulStr;
	        break;
	default:
		return nullptr;
	}

	return isUint256(_type) ? rv : nullptr;
}

string SECBITTransformer::getReplacement(
	ASTNode const& _node)
{
	auto i = m_replacements.find(_node.location());
	if(i == end(m_replacements)) {
		stringstream rv;
		getReplacement(
			_node.location(),
			rv);
		return rv.str();
	}
	string rv(move(i->second));
	m_replacements.erase(i);
	return rv;
}

void SECBITTransformer::endVisit(Assignment const& _node)
{
	if(!m_needReplace) {
		return;
	}
	// Replace operator with safemath call.
	if(auto *op = getOverflowOperator(
		   _node.assignmentOperator(),
		   _node.leftHandSide().annotation().type.get())) {
		string lhs = getReplacement(_node.leftHandSide());
		m_replacements.emplace(
			make_pair(
				_node.location(),
				lhs + " = " + lhs
				+ "." + op + "("
				+ getReplacement(_node.rightHandSide())
				+ ")"));
		m_hasReplace = true;
	}
}

void SECBITTransformer::endVisit(BinaryOperation const& _node)
{
	if(!m_needReplace) {
		return;
	}
	// Replace operator with safemath call.
	if(auto *op = getOverflowOperator(
		   _node.getOperator(),
		   _node.annotation().type.get())) {
		string left = getReplacement(_node.leftExpression());
		// Convert lhs to uint256 if needed.
		if(!isUint256(_node.leftExpression().annotation().type.get())) {
			left = "uint256(" + left + ")";
		}
		m_replacements.emplace(
			make_pair(
				_node.location(),
				left
				+ "." + op + "("
				+ getReplacement(_node.rightExpression())
				+ ")"));
		m_hasReplace = true;
	}
}

void SECBITTransformer::endVisit(FunctionCall const& _node)
{
	auto const* called = asC<MemberAccess>(&_node.expression());
	if(!called || called->memberName() != "pop") {
		return;
	}
        auto const* arrTy = asC<ArrayType>(called->expression().annotation().type.get());
	if(!(arrTy
	     && arrTy->isDynamicallySized()
	     && arrTy->location() == DataLocation::Storage)) {
		return;
	}
	string arr = getReplacement(called->expression());
	m_replacements.emplace(
		make_pair(
			_node.location(),
		        arr + ".length > 0 ? delete "
			+ arr + "[" + arr + ".length--] : ()"));
}

bool SECBITTransformer::visit(ForStatement const& _node)
{
	auto const* array = _node.array();

	if(!array) {
		return true;
	}

	// Transform a for-each loop.
	auto const* init =
		asC<VariableDeclarationStatement>(
			_node.initializationExpression());

	auto const* decl = init->declarations().front().get();

	string const& loopVarName = decl->name();

	string arrayText = getReplacement(*array);

	// For 'for(var x : arr) /body/',
	// add replacement of x -> arr[x].
	m_forEachLoopVarReplacement.emplace(
		make_pair(
			decl,
			arrayText + "[" + loopVarName + "]"));

	// Visit body.
	_node.body().accept(*this);

	// Remove this mapping.
	m_forEachLoopVarReplacement.erase(decl);

	// Change the loop to:
	// for(uint x = 0; x < arr.length; x++) /body[x/arr[x]]/
	m_replacements.emplace(
		make_pair(
			_node.location(),
			"for(uint "
			+ loopVarName + " = 0; "
			+ loopVarName + " < "
			+ arrayText + ".length; "
			// This won't overflow before out-of-gass.
			+ loopVarName + "++)"
			+ getReplacement(_node.body())));

	// Already visited.
	return false;
}

void SECBITTransformer::endVisit(Identifier const& _node)
{
	if(auto const* decl = _node.annotation().referencedDeclaration) {
		// If this is a loop var, replace it.
		auto i = m_forEachLoopVarReplacement.find(decl);
		if(i != end(m_forEachLoopVarReplacement)) {
			m_replacements.emplace(
				make_pair(
					_node.location(),
					i->second));
		}
	}
}

}
}

#endif
