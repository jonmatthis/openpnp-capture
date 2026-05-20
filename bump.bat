@echo off
for /f "tokens=2 delims=." %%n in ('git tag --sort=-version:refname -l "build.*" ^| cmd /c "more +0" 2^>nul') do set /a NEXT=%%n+1 & goto :tag
:: no tags found — start at 0
set NEXT=0
:tag
git tag build.%NEXT% && git push origin build.%NEXT%
echo Tagged and pushed: build.%NEXT%
