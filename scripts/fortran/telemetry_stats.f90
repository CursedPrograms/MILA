! MILA telemetry analyzer.
!
! Reads a CSV exported by the C++ / C# / F# controllers (EXPORT CSV button, saved to
! Documents\MILA-telemetry) and prints a statistics report: sample rate, time spent in each
! drive mode, per-sensor min / max / mean / standard deviation, outliers, connection gaps
! and close calls.
!
! BUILD
!   gfortran -O2 -Wall -Wextra -std=f2008 telemetry_stats.f90 -o telemetry_stats.exe
!
! USAGE
!   telemetry_stats.exe telemetry_20260921_113253.csv
!
! CSV columns:
!   timestamp,dist_cm,left_cm,right_cm,temp_c,hum_pct,speed_pct,mode,last_cmd
! Empty cells are readings the robot did not report (the firmware sends "---" for a missing
! side distance); they are counted as "missing" and left out of the statistics.

program telemetry_stats
  use, intrinsic :: iso_fortran_env, only: real64, error_unit
  implicit none

  integer, parameter :: dp = real64
  integer, parameter :: ncol = 6            ! numeric columns: dist, left, right, temp, hum, speed
  integer, parameter :: maxfields = 16
  integer, parameter :: maxmodes = 8
  real(dp), parameter :: close_call_cm = 20.0_dp
  real(dp), parameter :: gap_seconds = 2.0_dp
  real(dp), parameter :: outlier_sigma = 3.0_dp

  character(len=*), parameter :: labels(ncol) = [character(len=14) :: &
       'Front dist cm', 'Left dist cm', 'Right dist cm', 'Temp C', 'Humidity %', 'Speed %']

  character(len=1024) :: path, line
  character(len=64)   :: fields(maxfields), tmp
  character(len=16), allocatable :: modes(:)
  character(len=16)   :: mname(maxmodes)
  integer  :: mcount(maxmodes)
  real(dp), allocatable :: v(:, :), t(:)
  logical,  allocatable :: have(:, :)
  integer  :: unit, ios, nrows, n, nf, i, j, k, nmodes, ngaps, nclose
  real(dp) :: x, dt, longest_gap, total_s
  logical  :: ok

  ! ---- arguments ----
  call get_command_argument(1, path)
  if (len_trim(path) == 0) then
    write (error_unit, '(A)') 'usage: telemetry_stats <telemetry.csv>'
    stop 2
  end if

  open (newunit=unit, file=trim(path), status='old', action='read', iostat=ios)
  if (ios /= 0) then
    write (error_unit, '(2A)') 'cannot open ', trim(path)
    stop 1
  end if

  ! ---- pass 1: count data rows (skip the header) ----
  nrows = 0
  read (unit, '(A)', iostat=ios) line
  do
    read (unit, '(A)', iostat=ios) line
    if (ios /= 0) exit
    call strip_cr(line)
    if (len_trim(line) > 0) nrows = nrows + 1
  end do
  if (nrows == 0) then
    write (error_unit, '(A)') 'no data rows in file'
    stop 1
  end if

  allocate (v(nrows, ncol), have(nrows, ncol), t(nrows), modes(nrows))
  have = .false.
  v = 0.0_dp
  t = 0.0_dp
  modes = ''

  ! ---- pass 2: parse ----
  rewind (unit)
  read (unit, '(A)', iostat=ios) line
  n = 0
  do
    read (unit, '(A)', iostat=ios) line
    if (ios /= 0) exit
    call strip_cr(line)
    if (len_trim(line) == 0) cycle
    n = n + 1
    call split_csv(line, fields, nf)
    t(n) = epoch_seconds(fields(1))
    do j = 1, ncol
      if (j + 1 <= nf) then
        call parse_real(fields(j + 1), x, ok)
        if (ok) then
          v(n, j) = x
          have(n, j) = .true.
        end if
      end if
    end do
    if (nf >= 8) then
      tmp = adjustl(fields(8))
      modes(n) = tmp(1:len(modes))
    end if
  end do
  close (unit)

  ! ---- overview ----
  total_s = t(nrows) - t(1)
  write (*, '(A)') 'MILA telemetry report'
  write (*, '(2A)') 'File:      ', trim(path)
  write (*, '(A,I0)') 'Samples:   ', nrows
  write (*, '(A)', advance='no') 'Duration:  '
  call print_duration(total_s)
  if (nrows > 1) then
    write (*, '(3A)') 'Interval:  ', fstr(total_s / real(nrows - 1, dp), 2), ' s average between samples'
  end if

  ! time in each drive mode (by sample count)
  nmodes = 0
  mcount = 0
  do i = 1, nrows
    if (len_trim(modes(i)) == 0) cycle
    k = 0
    do j = 1, nmodes
      if (mname(j) == modes(i)) then
        k = j
        exit
      end if
    end do
    if (k == 0 .and. nmodes < maxmodes) then
      nmodes = nmodes + 1
      mname(nmodes) = modes(i)
      k = nmodes
    end if
    if (k > 0) mcount(k) = mcount(k) + 1
  end do
  if (nmodes > 0) then
    write (*, '(A)', advance='no') 'Modes:     '
    do j = 1, nmodes
      write (*, '(A,A,I0,A,A,A)', advance='no') trim(mname(j)), ' ', mcount(j), ' (', &
           fstr(100.0_dp * mcount(j) / nrows, 1), '%)  '
    end do
    write (*, *)
  end if

  ! ---- per-sensor statistics ----
  write (*, *)
  write (*, '(A14,A7,A9,A10,A10,A10,A10,A11)') 'Sensor', 'n', 'missing', 'min', 'max', 'mean', 'stddev', &
       'outliers'
  write (*, '(A)') repeat('-', 81)
  do j = 1, ncol
    call column_stats(j)
  end do

  ! ---- close calls, connection gaps ----
  write (*, *)
  nclose = 0
  do i = 1, nrows
    if (have(i, 1)) then
      if (v(i, 1) < close_call_cm) nclose = nclose + 1
    end if
  end do
  write (*, '(A,I0,A,I0,A,A,A)') 'Close calls (front < ', nint(close_call_cm), ' cm): ', nclose, &
       ' samples (', fstr(100.0_dp * nclose / nrows, 1), '%)'

  ngaps = 0
  longest_gap = 0.0_dp
  do i = 2, nrows
    dt = t(i) - t(i - 1)
    if (dt > gap_seconds) then
      ngaps = ngaps + 1
      longest_gap = max(longest_gap, dt)
    end if
  end do
  if (ngaps == 0) then
    write (*, '(3A)') 'Connection: no gaps longer than ', fstr(gap_seconds, 1), ' s'
  else
    write (*, '(A,I0,5A)') 'Connection: ', ngaps, ' gap(s) longer than ', fstr(gap_seconds, 1), &
         ' s (longest ', fstr(longest_gap, 1), ' s)'
  end if

contains

  ! Statistics for numeric column j. Two passes: mean, then deviations (numerically steadier than
  ! the sum-of-squares shortcut).
  subroutine column_stats(col)
    integer, intent(in) :: col
    integer  :: cnt, miss, out, r
    real(dp) :: mn, mx, sum, mean, ss, sd

    cnt = 0
    sum = 0.0_dp
    mn = huge(1.0_dp)
    mx = -huge(1.0_dp)
    do r = 1, nrows
      if (.not. have(r, col)) cycle
      cnt = cnt + 1
      sum = sum + v(r, col)
      mn = min(mn, v(r, col))
      mx = max(mx, v(r, col))
    end do
    miss = nrows - cnt
    if (cnt == 0) then
      write (*, '(A14,I7,I9,A)') labels(col), 0, miss, '        (no readings)'
      return
    end if

    mean = sum / real(cnt, dp)
    ss = 0.0_dp
    do r = 1, nrows
      if (have(r, col)) ss = ss + (v(r, col) - mean)**2
    end do
    sd = sqrt(ss / real(cnt, dp))

    out = 0
    if (sd > 0.0_dp) then
      do r = 1, nrows
        if (have(r, col)) then
          if (abs(v(r, col) - mean) > outlier_sigma * sd) out = out + 1
        end if
      end do
    end if
    write (*, '(A14,I7,I9,4F10.2,I11)') labels(col), cnt, miss, mn, mx, mean, sd, out
  end subroutine column_stats

  subroutine print_duration(seconds)
    real(dp), intent(in) :: seconds
    integer :: h, m, s
    s = nint(seconds)
    h = s / 3600
    m = mod(s, 3600) / 60
    s = mod(s, 60)
    write (*, '(I0,A,I2.2,A,I2.2,3A)') h, ':', m, ':', s, '  (', fstr(seconds, 1), ' s)'
  end subroutine print_duration

  ! A number with d decimals and no padding, keeping the leading zero (F0.d prints ".54").
  function fstr(x, d) result(s)
    real(dp), intent(in) :: x
    integer, intent(in) :: d
    character(len=:), allocatable :: s
    character(len=32) :: buf, fmt
    write (fmt, '(A,I0,A)') '(F24.', d, ')'
    write (buf, fmt) x
    s = trim(adjustl(buf))
  end function fstr

  subroutine strip_cr(s)
    character(len=*), intent(inout) :: s
    integer :: l
    l = len_trim(s)
    if (l > 0) then
      if (s(l:l) == achar(13)) s(l:l) = ' '   ! files written on Windows end lines with CR LF
    end if
  end subroutine strip_cr

  ! Split a comma-separated line into fields. Empty cells stay blank, and the field count is returned.
  subroutine split_csv(text, out, count)
    character(len=*), intent(in)  :: text
    character(len=*), intent(out) :: out(:)
    integer, intent(out) :: count
    integer :: p, start, l
    logical :: boundary

    out = ''
    count = 0
    start = 1
    l = len_trim(text)
    do p = 1, l + 1
      if (p > l) then
        boundary = .true.
      else
        boundary = (text(p:p) == ',')
      end if
      if (boundary) then
        count = count + 1
        if (count <= size(out) .and. p > start) out(count) = text(start:p - 1)
        start = p + 1
      end if
    end do
  end subroutine split_csv

  subroutine parse_real(s, value, good)
    character(len=*), intent(in) :: s
    real(dp), intent(out) :: value
    logical, intent(out) :: good
    integer :: status
    value = 0.0_dp
    good = .false.
    if (len_trim(s) == 0) return
    read (s, *, iostat=status) value
    good = (status == 0)
  end subroutine parse_real

  ! "YYYY-MM-DD HH:MM:SS.mmm" -> seconds since 1970-01-01 (local time; only differences matter).
  function epoch_seconds(ts) result(sec)
    character(len=*), intent(in) :: ts
    real(dp) :: sec
    integer :: y, mo, d, h, mi, status, era, yoe, doy, doe
    real(dp) :: s

    sec = 0.0_dp
    read (ts, '(I4,1X,I2,1X,I2,1X,I2,1X,I2,1X,F6.3)', iostat=status) y, mo, d, h, mi, s
    if (status /= 0) return

    ! days from civil date (H. Hinnant's algorithm), valid for the Gregorian calendar
    if (mo <= 2) y = y - 1
    era = y / 400
    yoe = y - era * 400
    if (mo > 2) then
      doy = (153 * (mo - 3) + 2) / 5 + d - 1
    else
      doy = (153 * (mo + 9) + 2) / 5 + d - 1
    end if
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy
    sec = real(era * 146097 + doe - 719468, dp) * 86400.0_dp + real(h * 3600 + mi * 60, dp) + s
  end function epoch_seconds

end program telemetry_stats
