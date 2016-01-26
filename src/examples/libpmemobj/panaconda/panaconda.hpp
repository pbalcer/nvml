/*
 * Copyright (c) 2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * panaconda.hpp -- example of usage c++ bindings in nvml
 */


#ifndef PANACONDA_HPP_
#define PANACONDA_HPP_

#include <libpmemobj.hpp>

//#############################################################################//
//									Forward decl
//#############################################################################//
class Element;


//#############################################################################//
//									Types
//#############################################################################//
typedef enum Direction
{
	UNDEFINED,
	DOWN,
	RIGHT,
	UP,
	LEFT
}Direction_t;

typedef enum ObjectType
{
	SNAKE_SEGMENT,
	WALL,
	FOOD
}ObjectType_t;

typedef enum ConfigFileSymbol
{
	SYM_NOTHING 	= '0',
	SYM_WALL		= '1'
}ConfigFileSymbol_t;

typedef enum SnakeEvent
{
	EV_OK,
	EV_COLLISION

}SnakeEvent_t;


typedef enum State
{
	STATE_NEW,
	STATE_PLAY,
	STATE_GAMEOVER
}State_t;

typedef enum Action
{
	ACTION_NEW_GAME = 'n',
	ACTION_QUIT		= 'q'

}Action_t;

typedef std::vector<
			pmem::persistent_ptr<Element>,
			pmem::pmem_allocator_basic<pmem::persistent_ptr<Element>>
		> ElementVector;

typedef std::vector<
			pmem::persistent_ptr< ElementVector >,
			pmem::pmem_allocator_basic<pmem::persistent_ptr< ElementVector >>
		> Element2DimVector;


//#############################################################################//
//									Classes
//#############################################################################//

typedef struct ColorPair
{
	ColorPair() : colorBg(COLOR_BLACK), colorFg(COLOR_BLACK) {}
	ColorPair(const int aColFg, const int aColBg) : colorBg(aColBg), colorFg(aColFg) {}
	int colorBg;
	int colorFg;
}ColorPair_t;


class Helper
{
	public:
		static inline void sleep( int aTime )
		{
			clock_t currTime = clock();
			while( clock() < (currTime + aTime) ) {}
		}
		static ColorPair_t getColor(const int aShape);
};



class Point
{
	public:
		pmem::p<int> mX;
		pmem::p<int> mY;

		Point() 				: mX( 0 ), mY( 0 ) 		{}
		Point( int aX, int aY) 	: mX( aX ), mY( aY ) 	{}

		friend bool operator== (Point &aPoint1, Point &aPoint2);
};

bool operator== (Point &aPoint1, Point &aPoint2)
{
	return ( aPoint1.mX == aPoint2.mX &&
			aPoint1.mY == aPoint2.mY );
}


class Shape
{
	public:
		Shape(int aShape);
		int getVal() { return mVal; }

	private:
		pmem::p<int> mVal;
		int getSymbol( int aShape);

};

class Element
{
	public:
		Element() : mPoint( pmem::make_persistent<Point>( 0, 0 ) ),
					mShape( pmem::make_persistent<Shape>( SNAKE_SEGMENT) ),
					mDirection( Direction_t::LEFT ) {}
		Element( int aX, int aY, pmem::persistent_ptr<Shape> aShape, Direction_t aDir ) :
					mPoint( pmem::make_persistent<Point>( aX, aY ) ),
					mShape( aShape ),
					mDirection( aDir ) {}
		Element( Point aPoint, pmem::persistent_ptr<Shape> aShape, Direction_t aDir ) :
					mPoint( pmem::make_persistent<Point>( aPoint.mX, aPoint.mY ) ),
					mShape( aShape ),
					mDirection( aDir ) {}
		Element (const Element &aElement)
		{
			mPoint = pmem::make_persistent<Point>( aElement.mPoint->mX, aElement.mPoint->mY);
			mShape = pmem::make_persistent<Shape>( aElement.mShape->getVal() );
		}

		~Element()
		{
			pmem::delete_persistent(mPoint);
			mPoint = nullptr;
			pmem::delete_persistent(mShape);
			mShape = nullptr;
		}

		pmem::persistent_ptr<Point> calcNewPosition( const Direction_t aDir );
		void print( void );
		void printDoubleCol( void );
		void printSingleDoubleCol( void );

		pmem::persistent_ptr<Point> getPosition( void );
		void setPosition(const pmem::persistent_ptr<Point> aNewPoint);
		Direction_t getDirection( void ) { return mDirection; }
		void setDirection( const Direction_t aDir ) { mDirection = aDir; }

	private:
		pmem::persistent_ptr<Point> mPoint;
		pmem::persistent_ptr<Shape> mShape;
		pmem::p<Direction_t> 		mDirection;

};

class Snake
{
	public:
		Snake();
		~Snake();

		void move( const Direction_t aDir );
		void print( void );
		void addSegment( void);
		bool checkPointAgainstSegments( Point aPoint);
		Point getHeadPoint( void );
		Direction_t getDirection( void );
		Point getNextPoint( const Direction_t aDir );


	private:
		ElementVector mSnakeSegments;
		pmem::p<Point> mLastSegPosition;
		pmem::p<Direction_t> mLastSegDir;

};

class Board
{
	public:
		Board();
		~Board();

		void print( const int aScore );
		void printGameOver( const int aScore );
		unsigned getSizeRow( void ) { return mSizeRow; }
		void setSizeRow(const unsigned aSizeRow) { mSizeRow = aSizeRow; }
		unsigned getSizeCol( void ) { return mSizeCol; }
		void setSizeCol(const unsigned aSizeCol) { mSizeCol = aSizeCol; }
		void creatDynamicLayout( const unsigned aRowNo, char * const aBuffer );
		void creatStaticLayout( void );
		bool isSnakeHeadFoodHit( void );
		void createNewFood( void );
		bool isCollision( Point aPoint );
		SnakeEvent_t moveSnake( const Direction_t aDir );
		Direction_t getSnakeDir( void ) { return mSnake->getDirection(); }
		void addSnakeSegment( void ) { mSnake->addSegment(); }


	private:
		pmem::persistent_ptr<Snake> mSnake;
		pmem::persistent_ptr<Element> mFood;
		//Element2DimVector mLayout;
		ElementVector mLayout;

		pmem::p<unsigned> mSizeRow;
		pmem::p<unsigned> mSizeCol;

		void setNewFood( const Point aPoint);
		bool isSnakeCollision( Point aPoint);
		bool isWallCollision( Point aPoint);

};



class Player
{
	public:
		Player() : mScore( 0 ), mState( STATE_PLAY ) {}
		~Player() {}
		int getScore( void ) { return mScore; }
		void updateScore( void );
		State_t getState( void ) { return mState; }
		void setState( const State_t aState ) { mState = aState; }

	private:
		pmem::p<int> mScore;
		pmem::p<State_t> mState;


};

class GameState
{
	public:
		GameState() {}
		~GameState() {}

		pmem::persistent_ptr<Board> getBoard() { return mBoard; }
		pmem::persistent_ptr<Player> getPlayer() { return mPlayer; }
		void init( void );
		void cleanPool( void );

	private:
		pmem::persistent_ptr<Board> mBoard;
		pmem::persistent_ptr<Player> mPlayer;
};

class Game
{
	public:
		Game( const std::string aName );
		~Game() {}

		void init( void );
		void initColors( void );
		void processStep( void );
		void processKey( const int aLastKey );
		inline bool isStopped( void );
		void delay( void );
		void clear( void );
		bool isGameOver( void );
		void gameOver( void );
		void closePool(void ) { mGameState.close(); }

	private:
		pmem::pool<GameState> mGameState;
		int mLastKey;
		int mDelay;

		Direction_t mDirectionKey;
		void setDirectionKey( void );
		void cleanPool( void );
		void parseConfCreatDynamicLayout( void );
};


#endif /* PANACONDA_HPP_ */
