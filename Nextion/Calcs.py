

screenWidth = 240
componentWidth = 100
componentHeight = 50

def positionComponentToCenter(screenWidth, componentWidth):
    return (screenWidth - componentWidth) // 2

positionComponentToCenter(screenWidth, componentWidth)


def nextOneDown(currentY, componentHeight, spacing=2):
    return currentY + componentHeight + spacing

currentY = 25 #First component Y position


heightIter = currentY

for i in range(5):
    nextY = nextOneDown(heightIter, componentHeight, spacing=80/4)
    print(f"{i} - Next component Y position: {nextY}")
    heightIter = nextY

len("""The time it takes
to get to the
first mode of 
transport in
minutes

The value should 
be a whole number

Example: 10""")

"""
if(b0.txt!="Loading..."&&b0.txt!="")
{
  if(b0.txt=="Transit")
  {
    page IfTransit
  } 
  else
  {
    page Walk
  }
}


# NEXTION CODE CRITERIA
1. No space between if and (
2. {} must always be on it's own line
"""